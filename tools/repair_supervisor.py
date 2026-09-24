"""Persist repair decisions, deterministic patches, verification and solver escalation"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from repair_contract import identity, local, load, native_sources, number, program, sha
from trace_worker import write_receipt


def diagnostic(result):
    detail=result.get('diagnostic') or {}
    path=detail.get('packet')
    if not path and result.get('report'):path=str(Path(result['report']).with_name('diagnostic-packet.json'))
    if not path or not Path(path).is_file():return None,None
    return Path(path),json.loads(Path(path).read_text())


def signature(packet):
    return identity({k:packet.get(k) for k in ('first_difference','terminal_failure','pc','ordinal','build')})


def matches(contract, packet):
    trigger=contract.get('trigger',{})
    if not trigger: return False
    if packet.get('first_difference'):
        return trigger.get('difference_sha256')==identity(packet['first_difference'])
    event=packet.get('event') or (packet.get('terminal_failure') or {}).get('event') or {}
    pc=packet.get('pc') or event.get('pc')
    if not pc or number(trigger.get('pc',-1)) != int(str(pc).removeprefix('0x'),16):return False
    if 'stage' in trigger and trigger['stage']!=event.get('stage'):return False
    if 'state' in trigger and trigger['state'] not in [a.get('state') for a in event.get('actors',[])]:return False
    return True


class Supervisor:
    def __init__(self, root, config, name, folder):
        self.root=Path(root);self.settings=config.get('stage_pipeline',{}).get('repair_supervisor',{})
        self.name=name;self.folder=Path(folder)/'repairs';self.folder.mkdir(parents=True,exist_ok=True)
        self.path=self.folder/'state.json'
        self.state=json.loads(self.path.read_text()) if self.path.exists() else dict(schema=1,attempts=[],solver_calls=0,pending=None)
        self.enabled=self.settings.get('enabled',False)

    def save(self):write_receipt(self.path,self.state)

    def prepare(self):
        if self.settings.get('validate_before_replay') and self.settings.get('validate_on_match'):
            raise ValueError('Choose validation before replay or after MATCH, not both')
        validation=None
        if self.settings.get('validate_before_replay',False):
            from trace_validation import ensure
            validation=ensure(self.name)
            self.state['pre_replay_validation']=validation;self.save()
            if validation['status']!='automated_checks_passed':return dict(status='validation_failed',validation=validation)
        if self.settings.get('solver',{}).get('mode')=='codex':
            from repair_solver import preflight
            self.state['solver_runtime']=preflight(self.settings['solver'],self.folder);self.save()
            if self.state['solver_runtime'].get('status')!='available':
                return dict(status='solver_unavailable',validation=validation,
                            solver_runtime=self.state['solver_runtime'])
        return dict(status='ready',validation=validation,solver_runtime=self.state.get('solver_runtime'))

    def refresh(self, directory):
        log=Path(directory)/'code-refresh.log'
        argv=[sys.executable,'-B',str(Path(__file__).with_name('xport.py')),'--project',str(self.root),'code_refresh']
        with log.open('w',encoding='utf-8') as output:
            code=subprocess.run(argv,cwd=self.root,stdout=output,stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0).returncode
        if code:raise RuntimeError('code_refresh failed: '+str(log))
        return dict(argv=argv,log=str(log),sha256=sha(log.read_bytes()))

    def apply(self, contract, candidate, directory):
        if contract['kind']!='project' or len(contract['native'].get('functions',[]))!=1:
            raise ValueError('Automatic application requires exactly one mapped project function')
        before,files=native_sources(self.root,contract)
        if len(files)!=1:raise ValueError('One-file patch required')
        relative=next(iter(files));path=local(self.root,relative)
        if not path.is_relative_to(self.root/'src') or path.suffix!='.c':raise ValueError('Only mapped game C is writable')
        raw=path.read_bytes();text=raw.decode('utf-8')
        newline='\r\n' if '\r\n' in text else '\n'
        original=before
        replacement=candidate['source'].replace('\r\n','\n').replace('\n',newline)
        if text.count(original)!=1 or sha(before.encode())!=candidate['before_sha256']:
            raise ValueError('Source changed before application')
        after=text.replace(original,replacement,1).encode('utf-8')
        backup=directory/'source-before.bin';backup.write_bytes(raw)
        pending=dict(status='intent',file=str(path),backup=str(backup),before_sha256=sha(raw),
                     applied_sha256=sha(after),directory=str(directory),ordinal=self.state.get('current_ordinal'))
        self.state['pending']=pending;self.save()
        if path.read_bytes()!=raw:raise ValueError('Source changed during application')
        temporary=path.with_name(path.name+'.xport-repair.tmp')
        if temporary.exists():raise ValueError('Ambiguous patch temporary file')
        with temporary.open('xb') as stream:stream.write(after);stream.flush();os.fsync(stream.fileno())
        temporary.replace(path)
        pending['status']='applied';self.save()
        pending['refresh']=self.refresh(directory)
        pending['applied_sha256']=sha(path.read_bytes());pending['status']='awaiting_full_replay';self.save()

    def recover(self):
        pending=self.state.get('pending')
        if not pending:return
        path=Path(pending['file']);actual=sha(path.read_bytes())
        if actual not in (pending['before_sha256'],pending['applied_sha256']):
            raise RuntimeError('Repair source changed outside supervisor; retain backup and inspect')
        if pending['status']=='intent' and actual==pending['before_sha256']:
            pending['status']='not_applied';self.state['pending']=None;self.save();return
        if pending['status']=='reverting':
            backup=Path(pending['backup']).read_bytes()
            if sha(backup)!=pending['before_sha256']:raise ValueError('Repair backup changed')
            if actual==pending['applied_sha256']:
                temporary=path.with_name(path.name+'.xport-repair.tmp')
                if temporary.exists():raise RuntimeError('Interrupted rollback temporary file requires ownership review')
                with temporary.open('xb') as stream:stream.write(backup);stream.flush();os.fsync(stream.fileno())
                temporary.replace(path)
            pending['refresh_after_revert']=self.refresh(Path(pending['directory']))
            pending['status']='reverted';write_receipt(Path(pending['directory'])/'patch-result.json',pending)
            self.state['pending']=None;self.save();return
        if pending['status'] in ('intent','applied'):
            pending['refresh']=self.refresh(Path(pending['directory']))
            pending['applied_sha256']=sha(path.read_bytes());pending['status']='awaiting_full_replay';self.save()

    def observe(self, result):
        pending=self.state.get('pending')
        if not pending:return
        if pending['status']!='awaiting_full_replay':raise RuntimeError('Unresolved patch transaction')
        if result.get('status') not in ('match','mismatch','capture_failed'):
            raise RuntimeError('Replay did not produce a semantic verdict; retain pending patch for explicit recovery')
        _,packet=diagnostic(result)
        ordinal=(packet or {}).get('ordinal')
        old=pending.get('ordinal')
        prefix=(packet or {}).get('verified_prefix_end')
        accepted=result.get('status')=='match' or (type(old) is int and type(ordinal) is int and ordinal>old
            and type(prefix) is int and prefix>old and not (packet or {}).get('prefix_problem'))
        if accepted:
            pending['status']='full_match' if result.get('status')=='match' else 'advanced_diagnostic_frontier'
            write_receipt(Path(pending['directory'])/'patch-result.json',pending)
            self.state['pending']=None;self.save();return
        path=Path(pending['file'])
        if sha(path.read_bytes())!=pending['applied_sha256']:raise RuntimeError('Cannot revert externally changed source')
        backup=Path(pending['backup']).read_bytes()
        if sha(backup)!=pending['before_sha256']:raise ValueError('Repair backup changed')
        pending['status']='reverting';self.save()
        temporary=path.with_name(path.name+'.xport-repair.tmp')
        with temporary.open('xb') as stream:stream.write(backup);stream.flush();os.fsync(stream.fileno())
        temporary.replace(path)
        pending['refresh_after_revert']=self.refresh(Path(pending['directory']))
        pending['status']='reverted_no_verified_progress';write_receipt(Path(pending['directory'])/'patch-result.json',pending)
        self.state['pending']=None;self.save()
        raise RuntimeError('Automatic patch did not advance the diagnostic frontier; restored owned source, solver review required')

    def escalate(self, packet, packet_path, directory, evidence=None):
        task=dict(schema=1,trace=self.name,failure_signature=signature(packet),diagnostic=str(packet_path),
                  diagnostic_sha256=sha(packet_path.read_bytes()),first_difference=packet.get('first_difference'),
                  terminal_failure=packet.get('terminal_failure'),audit=packet.get('audit'),
                  contract_reference=str(Path(__file__).with_name('REPAIR_CONTRACT.md')),
                  allowed_transformations=['callback_binding','state_transition'],evidence=evidence,
                  requested_output='Pinned project audit/repair contract or explicit missing causal observation')
        directory.mkdir(parents=True,exist_ok=True);write_receipt(directory/'solver-task.json',task)
        settings=self.settings.get('solver',{})
        if settings.get('mode','queue')!='codex' or self.state['solver_calls']>=int(settings.get('max_calls',2)):
            return dict(status='needs_solver',task=str(directory/'solver-task.json'))
        from repair_solver import solve, runtime
        _,_,environment=runtime(settings)
        startup=self.state.get('solver_runtime') or {}
        if startup.get('status')=='unavailable' and startup.get('fingerprint')==environment['fingerprint']:
            return dict(status='needs_solver',reason=startup.get('problem'),stderr_excerpt=startup.get('stderr_excerpt'),
                        evidence=str(self.folder/'solver-runtime.json'),suppressed_duplicate=True,
                        task=str(directory/'solver-task.json'))
        failure=self.state.get('solver_infrastructure_failure')
        if failure and failure.get('fingerprint')==environment['fingerprint']:
            return dict(status='needs_solver',reason=failure['reason'],suppressed_duplicate=True,
                        evidence=failure['evidence'],task=str(directory/'solver-task.json'))
        self.state['solver_calls']+=1;self.save()
        try:result=solve(self.root,directory/'solver',task,settings)
        except ValueError as error:
            if str(error)=='Solver task identity changed':
                self.state['solver_calls']-=1;self.save()
            raise
        if result.get('infrastructure_failure'):
            self.state['solver_infrastructure_failure']=dict(fingerprint=environment['fingerprint'],reason=result['reason'],
                evidence=str(directory/'solver/result.json'))
            self.save()
        return result

    def repair(self, result):
        try:return self._repair(result)
        except (ValueError, KeyError, AssertionError, OSError, subprocess.TimeoutExpired) as error:
            if self.state.get('pending'):raise
            packet_path,packet=diagnostic(result)
            if packet is None:return dict(status='needs_solver',reason=str(error))
            directory=self.folder/signature(packet)/'unsupported'
            solved=self.escalate(packet,packet_path,directory,dict(problem=str(error)))
            for attempt in self.state['attempts']:
                if attempt['signature']==signature(packet):attempt.update(status='needs_solver',error=str(error))
            self.save()
            return dict(status='needs_solver',reason=str(error),solver=solved,
                        scope='No source edit; unsupported bench/transform requires an enriched contract')

    def _repair(self, result):
        from repair_bench import run_contract
        from repair_dispatch import index, transform
        from repair_codegen import assess
        packet_path,packet=diagnostic(result)
        if packet is None:return dict(status='needs_solver',reason='No usable diagnostic packet')
        fingerprint=signature(packet)
        attempt=next((a for a in self.state['attempts'] if a['signature']==fingerprint),None)
        if attempt:
            from repair_solver import retryable_runtime_failure
            prior=attempt.get('solver') or {}
            if not retryable_runtime_failure(prior,self.settings.get('solver',{})):
                return dict(status='needs_solver',reason='Unchanged failure already investigated',state=str(self.path))
            if attempt.get('error')=='Solver task identity changed':
                self.state['solver_calls']=max(0,self.state['solver_calls']-1);attempt.pop('error')
            attempt['status']='retrying_solver_runtime';self.save()
        else:
            if len(self.state['attempts'])>=int(self.settings.get('max_repairs',8)):
                return dict(status='needs_solver',reason='Persistent repair budget exhausted',state=str(self.path))
            directory=self.folder/fingerprint;directory.mkdir(exist_ok=True)
            attempt=dict(signature=fingerprint,status='analyzing',directory=str(directory),started=time.time())
            self.state['attempts'].append(attempt);self.state['current_ordinal']=packet.get('ordinal');self.save()
        directory=Path(attempt['directory'])
        contracts=[]
        registry=self.settings.get('contracts_directory')
        if registry:
            registry=local(self.root,registry)
            if registry.exists():
                for path in sorted(registry.glob('*.json')):
                    _,contract=load(path)
                    if contract['kind']=='project' and matches(contract,packet):contracts.append(contract)
        if len(contracts)>1:
            attempt['status']='ambiguous_contracts';self.save()
            return dict(status='needs_solver',reason='Multiple matching repair contracts')
        if not contracts:
            solved=self.escalate(packet,packet_path,directory)
            answer=solved.get('answer') or {}
            if answer.get('decision')!='candidate':
                attempt['status']='needs_observation' if answer.get('decision')=='need_observation' else 'needs_solver'
                attempt['solver']=solved;self.save();return dict(status=attempt['status'],task=str(directory/'solver-task.json'),result=solved)
            contract=json.loads(answer['contract_json'])
            if contract.get('schema')!=1 or contract.get('kind')!='project' or not matches(contract,packet):
                raise ValueError('Solver contract is outside the exact observed failure')
            contracts=[contract];write_receipt(directory/'solver-contract.json',contract)
        contract=contracts[0];program(self.root,contract)
        write_receipt(directory/'codegen-assessment.json',assess(self.root,contract))
        baseline=run_contract(self.root,contract,directory/'baseline')
        if baseline['status']!='mismatch':
            attempt['status']='not_reproduced';self.save();return dict(status='needs_solver',reason='Contract does not reproduce a C mismatch',directory=str(directory))
        if not contract.get('repair'):
            observed=self.escalate(packet,packet_path,directory/'causal-escalation',
                                  dict(causal=str(directory/'baseline/causal.json'),report=str(directory/'baseline/report.json')))
            answer=observed.get('answer') or {}
            if answer.get('decision')!='candidate':
                attempt.update(status='needs_solver',solver=observed);self.save()
                return dict(status='needs_solver',causal=str(directory/'baseline/causal.json'),solver=observed)
            proposed=json.loads(answer['contract_json'])
            if not proposed.get('repair') or proposed.get('kind')!='project' or not matches(proposed,packet):
                raise ValueError('Causal solver did not return a scoped deterministic repair')
            # An enriched contract must reproduce independently before any edit
            contract=proposed
            baseline=run_contract(self.root,contract,directory/'enriched-baseline')
            if baseline['status']!='mismatch':raise ValueError('Enriched contract does not reproduce failure')
        write_receipt(directory/'dispatch-index.json',index(self.root,contract))
        candidate=transform(self.root,contract,contract['repair']);write_receipt(directory/'candidate.json',candidate)
        checked=run_contract(self.root,contract,directory/'candidate-check',candidate['source'])
        if checked['status']!='passed':
            attempt['status']='candidate_rejected';self.save();return dict(status='needs_solver',causal=str(directory/'candidate-check/causal.json'))
        self.apply(contract,candidate,directory)
        attempt['status']='applied_requires_full_replay';self.save()
        return dict(status='patched',directory=str(directory),full_verification_required=True)

    def finish(self, result):
        from trace_validation import preflight, run
        report_path=self.folder/'validation.json'
        validation=self.state.get('pre_replay_validation') if self.settings.get('validate_before_replay',False) else None
        if self.settings.get('validate_on_match',False):
            current=preflight()
            if report_path.exists():
                prior=json.loads(report_path.read_text())
                if prior['identity']==current:
                    if sha(Path(prior['result']['report']).read_bytes())!=prior['report_sha256']:
                        raise ValueError('Stored validation report changed')
                    validation=prior['result']
            if validation is None:
                validation=run(self.name);write_receipt(report_path,dict(identity=current,result=validation,
                    report_sha256=sha(Path(validation['report']).read_bytes())))
            if validation.get('status')!='automated_checks_passed':
                return dict(status='validation_failed',validation=validation)
        completion=dict(schema=1,status='complete',trace=self.name,match=result,validation=validation,
                        validation_timing='before_replay' if self.settings.get('validate_before_replay') else 'after_match' if self.settings.get('validate_on_match') else 'not_requested',
                        attempts=len(self.state['attempts']),solver_calls=self.state['solver_calls'],
                        scope='Full runner MATCH plus separately scoped automated validation; solver sessions retain own usage')
        write_receipt(self.folder/'completion.json',completion)
        return dict(status='complete',report=str(self.folder/'completion.json'),validation=validation)
