#!/usr/bin/env python3
"""Run the pinned lwIP RTT estimator and timeout comparisons with project ticks.

The unchanged SDK initialization, estimator and expiry expressions execute with
controlled ACK timing. This models established TCP loss-recovery latency; it
does not inject packet loss or claim to measure the bench network.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.1/esp-idf'))

PREFIX = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int16_t s16_t;
typedef uint8_t u8_t;
typedef uint16_t u16_t;
typedef uint32_t u32_t;
#define LWIP_DEBUGF(...) do {} while(0)
struct tcp_pcb {
    s16_t sa,sv,rto;
    u32_t rttest,tmr,keep_idle,keep_intvl,keep_cnt,keep_cnt_sent;
};
u32_t tcp_ticks;
void check(int condition,const char *message) {
    if(!condition){fprintf(stderr,"FAIL %s\n",message);exit(1);}
}
'''

SUITE = r'''
void check_timeout(int (*expired)(struct tcp_pcb *),u32_t nominal_ms) {
    struct tcp_pcb pcb;initialize(&pcb);pcb.tmr=100;
    pcb.keep_idle=TCP_KEEPIDLE_DEFAULT;pcb.keep_intvl=TCP_KEEPINTVL_DEFAULT;
    pcb.keep_cnt=TCP_KEEPCNT_DEFAULT;pcb.keep_cnt_sent=0;
    check(nominal_ms%TCP_SLOW_INTERVAL==0,"nominal protocol timeout was rounded");
    tcp_ticks=pcb.tmr+nominal_ms/TCP_SLOW_INTERVAL;
    check(!expired(&pcb),"protocol timeout expired before its nominal duration");
    ++tcp_ticks;
    check(expired(&pcb),"protocol timeout did not expire on its next slow tick");
}
int main(void) {
    struct tcp_pcb pcb;initialize(&pcb);
    check(pcb.rto*TCP_SLOW_INTERVAL==1500,"initial retransmission timeout changed");
    check(TCP_FAST_INTERVAL==CONFIG_LWIP_TCP_TMR_INTERVAL,"fast timer scaling changed");
    check(TCP_SLOW_INTERVAL==2*CONFIG_LWIP_TCP_TMR_INTERVAL,"slow timer scaling changed");
    const unsigned close_budget=close_poll_budget();
    check(close_budget<=UINT8_MAX&&(u8_t)close_budget==close_budget,"netconn close poll counter overflowed");
    check((close_budget-1)*TCP_SLOW_INTERVAL==LWIP_TCP_CLOSE_TIMEOUT_MS_DEFAULT,"netconn close duration changed");
    check_timeout(fin_expired,TCP_FIN_WAIT_TIMEOUT);
    check_timeout(syn_expired,TCP_SYN_RCVD_TIMEOUT);
    check_timeout(last_ack_expired,2*TCP_MSL);
    check_timeout(keepalive_probe,TCP_KEEPIDLE_DEFAULT);
    check_timeout(keepalive_expired,TCP_KEEPIDLE_DEFAULT+TCP_KEEPCNT_DEFAULT*TCP_KEEPINTVL_DEFAULT);
    printf("PASS %dms ticks retain initial RTO1500ms, FIN/SYN20s, 2MSL120s and keepalive durations\n",CONFIG_LWIP_TCP_TMR_INTERVAL);
    printf("PASS TCP servicing%ums, close servicing%ums, close poll budget%u fits u8\n",
           NETCONN_TCP_POLL_INTERVAL*TCP_SLOW_INTERVAL,TCP_SLOW_INTERVAL,close_budget);

    // Sparse replies within a single slow tick must learn a useful loss
    // recovery interval rather than retain the 1500 ms initial variance seed.
    for(unsigned sample=0;sample<200;++sample)ack(&pcb,0);
    check(pcb.sa==0&&pcb.sv==3,"pinned low-RTT estimator behavior changed");
    printf("Established zero-tick ACK RTO: %dms\n",pcb.rto*TCP_SLOW_INTERVAL);
    check(pcb.rto*TCP_SLOW_INTERVAL<=300,"established zero-tick ACK RTO exceeds300ms");

    // Low RTT replies straddling timer boundaries measure zero or one tick.
    // Both orders exercise integer-variance updates and a persistent history.
    for(unsigned pattern=0;pattern<2;++pattern) {
        initialize(&pcb);
        for(unsigned sample=0;sample<200;++sample)ack(&pcb,0);
        unsigned maximum=0;
        for(unsigned sample=0;sample<1000;++sample) {
            ack(&pcb,(sample+pattern)%2);
            const unsigned rto_ms=pcb.rto*TCP_SLOW_INTERVAL;
            if(rto_ms>maximum)maximum=rto_ms;
            check(rto_ms>0&&rto_ms<=500,"one-tick ACK jitter exceeds500ms recovery budget");
        }
        printf("PASS low-RTT boundary jitter order%u: maximum RTO%ums\n",pattern,maximum);
    }
    return 0;
}
'''


def setting(source, name):
    match = re.search(r'^' + re.escape(name) + r'=(\d+)$', source, re.MULTILINE)
    assert match, 'Missing explicit TCP setting: ' + name
    return int(match[1])


def macro(source, name):
    match = re.search(r'^#define\s+' + re.escape(name) + r'(?=\s|\().*$', source, re.MULTILINE)
    assert match, 'Missing SDK macro: ' + name
    return match[0] + '\n'


def timeout_condition(source, threshold):
    position = source.index(threshold)
    start = source.rindex('if (', 0, position) + len('if (')
    depth, end = 1, start
    while depth:
        if source[end] == '(':
            depth += 1
        elif source[end] == ')':
            depth -= 1
        end += 1
    return source[start:end - 1]


def actual_source():
    lwip = IDF / 'components/lwip'
    core = lwip / 'lwip/src/core'
    tcp = (core / 'tcp.c').read_text()
    incoming = (core / 'tcp_in.c').read_text()
    private = (lwip / 'lwip/src/include/lwip/priv/tcp_priv.h').read_text()
    options = (lwip / 'port/include/lwipopts.h').read_text()
    netconn = (lwip / 'lwip/src/api/api_msg.c').read_text()
    api = (lwip / 'lwip/src/api/api_lib.c').read_text()
    default_options = (lwip / 'lwip/src/include/lwip/opt.h').read_text()
    api_types = (lwip / 'lwip/src/include/lwip/priv/api_msg.h').read_text()
    result = ''.join(macro(options, name) for name in ('TCP_TMR_INTERVAL', 'LWIP_TCP_RTO_TIME'))
    result += ''.join(macro(private, name) for name in (
        'TCP_FAST_INTERVAL', 'TCP_SLOW_INTERVAL', 'TCP_FIN_WAIT_TIMEOUT',
        'TCP_SYN_RCVD_TIMEOUT', 'TCP_MSL', 'TCP_KEEPIDLE_DEFAULT',
        'TCP_KEEPINTVL_DEFAULT', 'TCP_KEEPCNT_DEFAULT'))
    result += macro(tcp, 'TCP_KEEP_DUR') + macro(tcp, 'TCP_KEEP_INTVL')
    result += macro(netconn, 'NETCONN_TCP_POLL_INTERVAL')
    result += macro(default_options, 'LWIP_TCP_CLOSE_TIMEOUT_MS_DEFAULT')
    close_budget = re.search(r'API_MSG_VAR_REF\(msg\)\.msg\.sd\.polls_left =\s*([^;]+);', api)
    assert close_budget and 'u8_t polls_left;' in api_types, 'Pinned close poll counter changed'
    assert 'tcp_poll(tpcb, poll_tcp, 1);' in netconn, 'Pinned close polling interval changed'
    result += 'unsigned close_poll_budget(void) {return ' + close_budget[1] + ';}\n'
    initial = re.search(r'    pcb->rto = LWIP_TCP_RTO_TIME / TCP_SLOW_INTERVAL;\s*'
                        r'    pcb->sv = LWIP_TCP_RTO_TIME / TCP_SLOW_INTERVAL;', tcp)
    assert initial, 'Pinned RTO initialization changed'
    result += 'void initialize(struct tcp_pcb *pcb) {\nmemset(pcb,0,sizeof(*pcb));\n' + initial[0] + '\n}\n'
    start = incoming.index('      m = (s16_t)(tcp_ticks - pcb->rttest);')
    end = incoming.index('      pcb->rttest = 0;', start) + len('      pcb->rttest = 0;')
    result += ('void ack(struct tcp_pcb *pcb,u32_t elapsed_ticks) {\ns16_t m;\n'
               'pcb->rttest=100;tcp_ticks=100+elapsed_ticks;\n' + incoming[start:end] + '\n}\n')
    thresholds = {
        'fin_expired': 'TCP_FIN_WAIT_TIMEOUT / TCP_SLOW_INTERVAL',
        'syn_expired': 'TCP_SYN_RCVD_TIMEOUT / TCP_SLOW_INTERVAL',
        'last_ack_expired': '2 * TCP_MSL / TCP_SLOW_INTERVAL',
        'keepalive_probe': '(pcb->keep_idle + pcb->keep_cnt_sent * TCP_KEEP_INTVL(pcb))',
        'keepalive_expired': '(pcb->keep_idle + TCP_KEEP_DUR(pcb)) / TCP_SLOW_INTERVAL',
    }
    for name, threshold in thresholds.items():
        result += 'int ' + name + '(struct tcp_pcb *pcb) {return ' + timeout_condition(tcp, threshold) + ';}\n'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--defaults-source', type=Path, default=ROOT / 'sdkconfig.defaults')
    args = parser.parse_args()
    defaults = args.defaults_source.read_text()
    interval = setting(defaults, 'CONFIG_LWIP_TCP_TMR_INTERVAL')
    initial_rto = setting(defaults, 'CONFIG_LWIP_TCP_RTO_TIME')
    assert interval > 0 and initial_rto == 1500, 'Keep positive TCP ticks and the1500ms initial RTO'
    actual = actual_source()
    with tempfile.TemporaryDirectory(prefix='amity-lwip-timer-') as directory:
        base = Path(directory)
        source = base / 'test.c'
        source.write_text(PREFIX + actual + SUITE)
        for tick_ms, legacy in ((250, True), (interval, False)):
            binary = base / ('legacy' if legacy else 'configured')
            subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
                '-std=c99', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                '-DCONFIG_LWIP_TCP_TMR_INTERVAL=' + str(tick_ms),
                '-DCONFIG_LWIP_TCP_RTO_TIME=' + str(initial_rto), str(source), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            if legacy:
                assert result.returncode != 0 and 'established zero-tick ACK RTO exceeds300ms' in result.stderr, result
                assert 'Established zero-tick ACK RTO: 1500ms' in result.stdout, result
                print('Reproduced old250ms timer: established ACK estimator retains1500ms RTO')
            else:
                print(result.stdout, end='')
                if result.returncode:
                    print(result.stderr, end='')
                result.check_returncode()


if __name__ == '__main__':
    main()
