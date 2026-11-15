#!/usr/bin/env python3
"""Generate the pinned mDNS responder source with delegated-host goodbyes.

Removing a delegated hostname frees its address records without telling the
network, so peers keep resolving the old name until their caches expire. Send
TTL-zero A/AAAA records for the host before it is freed (RFC 6762, 10.1). The
managed component stays untouched; CMake compiles this generated copy in the
same mDNS target.
"""
from pathlib import Path
import argparse
import hashlib


# espressif/mdns 1.13.1 helper, from its declaration through the closing brace,
# with LF line endings and one trailing newline. Review the entire helper and
# the goodbye below on component updates.
PINNED_HELPER_SHA256 = '51f69c131743565b915dfa6c5abd69c95ea4ad1b6fc3838a6fe5447207b206e5'
SIGNATURE = 'static bool delegate_hostname_remove(const char *hostname)'

INCLUDE = '#include "mdns_service.h"\n'
NETWORKING = '#include "mdns_networking.h"\n'

GOODBYE = '''/**
 * @brief  Withdraw a delegated host's address records before it is removed
 */
static void send_delegated_host_bye(mdns_host_item_t *host)
{
    if (!host->address_list || mdns_utils_str_null_or_empty(s_server->hostname)) {
        return;
    }
    for (int i = 0; i < MDNS_MAX_INTERFACES; i++) {
        for (int j = 0; j < MDNS_IP_PROTOCOL_MAX; j++) {
            if (!mdns_priv_if_ready((mdns_if_t) i, (mdns_ip_protocol_t) j)) {
                continue;
            }
            mdns_tx_packet_t *packet = mdns_priv_alloc_packet((mdns_if_t) i, (mdns_ip_protocol_t) j);
            if (!packet) {
                continue;
            }
            packet->flags = MDNS_FLAGS_QR_AUTHORITATIVE;
            if (mdns_priv_create_answer(&packet->answers, MDNS_TYPE_A, NULL, host, true, true)
                    && mdns_priv_create_answer(&packet->answers, MDNS_TYPE_AAAA, NULL, host, true, true)) {
                mdns_priv_dispatch_tx_packet(packet);
            }
            mdns_priv_free_tx_packet(packet);
        }
    }
}

'''

ORIGINAL = '''        if (strcasecmp(hostname, host->hostname) == 0) {
            if (prev_host == NULL) {
                s_server->host_list = host->next;'''
CORRECTED = '''        if (strcasecmp(hostname, host->hostname) == 0) {
            send_delegated_host_bye(host);
            if (prev_host == NULL) {
                s_server->host_list = host->next;'''


def patch_source(source: str) -> str:
    if source.count(SIGNATURE) != 1 or source.count(INCLUDE) != 1 or NETWORKING in source \
            or 'send_delegated_host_bye' in source:
        raise ValueError('Unrecognized mDNS responder source; review the delegated-host goodbye')
    start = source.index(SIGNATURE)
    brace = source.find('{', start)
    if brace < 0:
        raise ValueError('Unrecognized mDNS delegated-host removal declaration')
    depth, end = 1, brace + 1
    while depth and end < len(source):
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    helper = source[start:end]
    if depth or hashlib.sha256((helper.replace('\r\n', '\n') + '\n').encode()).hexdigest() != PINNED_HELPER_SHA256:
        raise ValueError('mDNS delegated-host removal changed; review the complete goodbye implementation')
    if helper.count(ORIGINAL) != 1:
        raise ValueError('Unrecognized mDNS delegated-host removal path')
    corrected = GOODBYE + helper.replace(ORIGINAL, CORRECTED, 1)
    return (source[:start] + corrected + source[end:]).replace(INCLUDE, INCLUDE + NETWORKING, 1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    corrected = patch_source(args.input.read_bytes().decode())
    if args.input.resolve() == args.output.resolve():
        parser.error('The generated source must not replace the managed component source')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_bytes() != corrected.encode():
        args.output.write_bytes(corrected.encode())


if __name__ == '__main__':
    main()
