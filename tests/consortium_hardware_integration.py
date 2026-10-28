#!/usr/bin/env python3
"""Exercise an independently implemented BLE candidate/TLS peer on a reserved Pi.

The two peer programs must already be installed with the user's authorization.
No source, existing credentials, real CEC actuators or pairing data are copied.
Temporary test membership is explicitly revoked and portable config restored.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid

from cec_web_activity import Admin, read_password, write_private


def candidate_token(admission, scan_id, label):
    """Select only this run's advertised fixture; never the first nearby board."""
    if admission.get('scan_id') != scan_id:
        raise AssertionError('Admission scan changed during fixture selection')
    if admission.get('requires_setup_code') is not False:
        raise AssertionError('The passive fixture requires the normal admission build')
    matches = [item for item in admission.get('candidates', []) if item.get('label') == label]
    if not matches:
        return None
    if len(matches) != 1 or not isinstance(matches[0].get('token'), str) or not 1 <= len(matches[0]['token']) <= 128:
        raise AssertionError('Fixture candidate selection is ambiguous or invalid')
    return matches[0]['token']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', required=True)
    parser.add_argument('--credentials', required=True, type=Path)
    parser.add_argument('--host', default='pi@10.0.1.119')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--seconds', default=600, type=int)
    args = parser.parse_args()
    if not 60 <= args.seconds <= 1200:
        parser.error('Choose a bounded 60–1200 second test')
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
           '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null', args.host]
    remote = '/tmp/amity-consortium-peer/run-' + uuid.uuid4().hex[:12]
    report = {'passed': [], 'remote_directory': remote, 'api': [], 'peer': {}}
    admin = Admin(args.url)
    report['api'] = admin.records
    original = None
    peer_id = None
    process = None
    log = None
    removed = False
    scan_id = None

    def run(*command):
        result = subprocess.run(ssh + [shlex.join(command)], capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise RuntimeError('Harness command failed: ' + command[0] + ': ' + result.stderr.strip())
        return result.stdout

    def peer_result():
        result = subprocess.run(ssh + [shlex.join(['cat', remote + '/result.json'])],
                                capture_output=True, text=True, timeout=15)
        if result.returncode:
            return None
        value = json.loads(result.stdout)
        report['peer'] = value
        return value

    def check(name, condition):
        if not condition:
            raise AssertionError(name)
        report['passed'].append(name)
        print('PASS', name, flush=True)

    def mutation(path, **body):
        return admin.request('/api/v2/' + path, {'request_id': uuid.uuid4().hex, **body})

    def memory_sample():
        status = admin.request('/api/status')
        sample = {key: status.get(key) for key in ('free_heap', 'free_internal_heap', 'largest_internal_block')}
        if not all(isinstance(value, int) and value >= 0 for value in sample.values()):
            raise AssertionError('DUT did not expose valid memory measurements')
        return sample

    def wait_operation(operation, timeout=90):
        deadline = time.monotonic() + timeout
        while operation['stage'] in ('admitted', 'configuring', 'preparing', 'activating', 'executing'):
            if time.monotonic() >= deadline:
                break
            time.sleep(.5)
            operation = admin.request('/api/v2/operations/' + operation['operation_id'])
        report.setdefault('operations', []).append(operation)
        return operation

    def settled(timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = admin.request('/api/v2/system')
            if state['ready'] and not state['configuration_pending'] and not state['discovering']:
                return state
            time.sleep(1)
        raise AssertionError('DUT did not settle')

    def remove_peer():
        nonlocal removed
        reviewed = admin.request('/api/v2/system')['membership']
        target = next((peer for peer in reviewed['peers'] if peer['octopus_id'] == peer_id), {})
        result = mutation('membership/remove', octopus_id=peer_id, confirm=True,
                          expected_membership_revision=reviewed['membership_revision'],
                          expected_target_boot_id=target.get('boot_id'))
        removed = True  # Accepted removal has already durably dropped the peer.
        result = wait_operation(result, timeout=8)
        report['removal_operation'] = result
        return result

    def portable(value):
        return {k: v for k, v in value.items() if k not in ('config_revision', 'presentation_revision')}

    def cleanup_attempt(step, action):
        try:
            return action()
        except Exception as error:
            report.setdefault('cleanup_errors', []).append({'step': step, 'error': str(error)})
            return None

    def restore_configuration():
        current = admin.request('/api/v2/config')
        if portable(current) != portable(original):
            result = mutation('config', configuration=original,
                              expected_config_revision=current['config_revision'],
                              expected_presentation_revision=current['presentation_revision'])
            result = wait_operation(result)
            if result['stage'] != 'applied':
                raise AssertionError('Configuration restoration did not apply: ' + str(result))

    def stop_fixture():
        if process is None or report.get('fixture_exited'):
            return
        try:
            run('touch', remote + '/stop')
        except Exception as error:
            report['fixture_stop_error'] = str(error)
        try:
            process.wait(timeout=15)
            report['fixture_exited'] = process.returncode == 0
        except Exception as error:
            report['fixture_wait_error'] = str(error)

    def revoke_if_present():
        state = admin.request('/api/v2/system')
        if any(p['octopus_id'] == peer_id for p in state['membership']['peers']):
            remove_peer()
        else:
            report['no_test_membership_to_revoke'] = True

    try:
        admin.login(read_password(args.credentials))
        state = settled()
        original = admin.request('/api/v2/config')
        report['before'] = {'system': state, 'configuration': original,
                            'setup': admin.request('/api/setup'),
                            'memory': memory_sample(),
                            'remote_status': admin.request('/api/remotes')}
        write_private(args.output.with_name(args.output.stem + '-before.json'), report['before'])
        check('DUT starts healthy and idle', not state['sessions'])
        run('mkdir', '-m', '700', remote)
        log_path = args.output.with_name(args.output.stem + '-peer.log')
        fd = os.open(log_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        log = os.fdopen(fd, 'wb', buffering=0)
        command = ['/tmp/amity-consortium-peer-venv/bin/python', '-u',
                   '/tmp/amity-consortium-peer/tests/consortium_hardware_peer.py',
                   '--state', remote + '/state.json',
                   '--result', remote + '/result.json', '--seconds', str(args.seconds + 60),
                   '--faults', '--stop-file', remote + '/stop']
        process = subprocess.Popen(ssh + [shlex.join(command)], stdout=log, stderr=log)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise AssertionError('Peer fixture exited before registration; inspect private peer log')
            result = peer_result()
            if result and any(e['event'] == 'own_gatt_advertisement_registered' for e in result['events']):
                peer_id = result['peer_id']
                break
            time.sleep(.5)
        check('independent GATT candidate registered without adapter changes', peer_id is not None)
        scan = mutation('membership/scan')
        scan_id = scan['operation_id']
        check('main accepts this run’s discovery request', scan['stage'] == 'complete')
        deadline = time.monotonic() + 35
        token = None
        while time.monotonic() < deadline:
            admission = admin.request('/api/v2/membership/admission')
            token = candidate_token(admission, scan_id, 'Amity ' + peer_id[-6:])
            if token:
                break
            if admission.get('phase') in ('failed', 'cancelled'):
                raise AssertionError('Candidate discovery did not complete')
            time.sleep(.5)
        check('main discovers the exact advertised fixture without an Add secret', token is not None)
        operation = mutation('membership/add', scan_id=scan_id, candidate_token=token,
                             name='bench-member-' + peer_id[-8:])
        report['add_admission'] = operation
        print('Enrollment admitted; waiting for independent peer checks', flush=True)
        deadline = time.monotonic() + args.seconds
        next_operation_probe = 0
        while time.monotonic() < deadline:
            result = peer_result()
            if process.poll() is not None:
                raise AssertionError('Peer fixture exited during test; inspect private peer log')
            if result and result.get('qualified'):
                break
            if time.monotonic() >= next_operation_probe:
                operation = admin.request('/api/v2/operations/' + operation['operation_id'])
                report['add_progress'] = operation
                if operation['stage'] in ('failed', 'rejected'):
                    raise AssertionError('Enrollment failed: ' + (operation.get('error') or operation.get('message') or operation['stage']))
                report.setdefault('memory_samples', []).append(memory_sample())
                next_operation_probe = time.monotonic() + 5
            time.sleep(2)
        check('independent peer completes its bounded fault sequence', bool(result and result.get('qualified')))
        events = {e['event'] for e in result['events']}
        for event in ['ephemeral_admission_confirmed', 'encrypted_identity_requested',
                      'prepared_after_authenticated_proposal', 'mutual_tls_lan_commit',
                      'bidirectional_hashed_snapshot_sync', 'operational_ready_and_execution_leases',
                      'tls_rejected_no_certificate', 'tls_rejected_unknown',
                      'session_rejected_wrong_boot', 'session_rejected_wrong_destination',
                      'session_rejected_wrong_consortium', 'session_rejected_wrong_config',
                      'session_rejected_oversized_frame', 'session_rejected_duplicate_json_key']:
            check(event, event in events)
        operation = wait_operation(operation)
        check('Add completes for the exact candidate', operation['stage'] == 'complete')
        state = settled()
        peer = next((p for p in state['membership']['peers'] if p['octopus_id'] == peer_id), {})
        check('authenticated peer is ready on current applied configuration',
              peer.get('ready') and peer.get('enrolled')
              and state['applied_config_revision'] == admin.request('/api/v2/config')['config_revision'])
        check('membership validation creates no activity session', not state['sessions'])
        removal = remove_peer()
        check('main drops the synthetic peer and confirms its durable reset decision',
              removal['stage'] == 'complete' and removal.get('reset') == 'scheduled')
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            result = peer_result()
            if result and any(e['event'] == 'synthetic_factory_reset_completed' for e in result['events']):
                break
            time.sleep(1)
        check('synthetic peer commits reset before acknowledging it',
              any(e['event'] == 'authenticated_factory_reset_acknowledged' for e in report['peer'].get('events', [])))
        check('only fixture-private state completes its synthetic factory reset',
              any(e['event'] == 'synthetic_factory_reset_completed' for e in report['peer'].get('events', [])))
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error) or type(error).__name__
        raise
    finally:
        if report.get('error'):
            if scan_id:
                def cancel_scan():
                    admission = admin.request('/api/v2/membership/admission')
                    if admission.get('scan_id') == scan_id and admission.get('phase') not in ('idle', 'complete', 'failed', 'cancelled'):
                        mutation('membership/cancel', scan_id=scan_id)
                cleanup_attempt('cancel_admission', cancel_scan)
            # Stop this failed candidate before inspecting its durable roster
            # entry; no late preparation can race the cleanup inspection.
            stop_fixture()
        if original is not None:
            # A failed removal must not prevent restoring configuration or
            # collecting the independently available final-state evidence.
            if peer_id and not removed:
                cleanup_attempt('remove_test_peer', revoke_if_present)
            if peer_id:
                cleanup_attempt('read_peer_result', peer_result)
            cleanup_attempt('restore_configuration', restore_configuration)
            state = cleanup_attempt('settle_dut', settled)
            current = cleanup_attempt('read_final_configuration', lambda: admin.request('/api/v2/config'))
            remote_status = cleanup_attempt('read_final_remote_status', lambda: admin.request('/api/remotes'))
            memory = cleanup_attempt('read_final_memory', memory_sample)
            report['after'] = {'system': state, 'configuration': current, 'remote_status': remote_status, 'memory': memory}
            if current is not None:
                report['configuration_restored'] = portable(current) == portable(original)
            if state is not None:
                report['idle'] = not state['sessions']
                report['no_enrolled_test_peer'] = not any(p['octopus_id'] == peer_id and p.get('enrolled') for p in state['membership']['peers'])
                report['no_test_peer_remaining'] = not any(p['octopus_id'] == peer_id for p in state['membership']['peers'])
            if remote_status is not None:
                before_bond = report['before']['remote_status']
                report['bonded_state_preserved'] = (isinstance(before_bond.get('bonded'), bool)
                                                    and before_bond['bonded'] == remote_status.get('bonded'))
                report['bond_identity_evidence_limit'] = ('The remote-status API exposes bond presence but no bonded-peer identity. '
                                                         'This comparison does not prove exact bond identity preservation.')
        if process is not None:
            stop_fixture()
            if log:
                log.close()
        if admin.token:
            try:
                admin.request('/api/logout', {})
            except Exception:
                pass
        write_private(args.output, report)
    assert (report.get('configuration_restored') and report.get('idle') and report.get('bonded_state_preserved')
            and report.get('no_enrolled_test_peer') and report.get('no_test_peer_remaining') and report.get('fixture_exited')
            and not report.get('fixture_stop_error') and not report.get('fixture_wait_error')
            and not report.get('cleanup_errors')), 'Final fixture cleanup check failed'
    print('PASS original configuration, idle and bonded state preserved; test membership revoked and fixture exited', flush=True)


if __name__ == '__main__':
    main()
