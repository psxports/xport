"""Request one tool-free JSON repair proposal through the Responses API"""

from __future__ import annotations

import json
import os
from pathlib import Path
from urllib import request

from lockstep_repair_context import build_prompt
from lockstep_supervisor import shared_source_identity, source_snapshot


RESPONSE_URL = 'https://api.openai.com/v1/responses'
MAX_RESPONSE_BYTES = 1024 * 1024
TIMEOUT_SECONDS = 300


def proposal_schema() -> dict:
    coverage = {
        'type': 'object', 'additionalProperties': False,
        'properties': {
            'check_range': {'type': 'array', 'items': {'type': 'integer'}},
            'source_file': {'type': 'string'},
            'source_symbol': {'type': 'string'},
            'source_line': {'type': 'integer'},
        },
        'required': ['check_range', 'source_file', 'source_symbol', 'source_line'],
    }
    audit = {
        'type': 'object', 'additionalProperties': False,
        'properties': {
            'image': {'type': 'string'},
            'address': {'type': 'integer'},
            'pseudocode_used_as_baseline': {'type': 'boolean'},
            'complete_mips_audit': {'type': 'boolean'},
            'unresolved_semantics': {'type': 'array', 'items': {'type': 'string'}},
            'machine_coverage': {'type': 'array', 'items': coverage},
        },
        'required': ['image', 'address', 'pseudocode_used_as_baseline',
                     'complete_mips_audit', 'unresolved_semantics', 'machine_coverage'],
    }
    edit = {
        'type': 'object', 'additionalProperties': False,
        'properties': {key: {'type': 'string'} for key in ('path', 'before', 'after')},
        'required': ['path', 'before', 'after'],
    }
    memory_exception = {
        'type': 'object', 'additionalProperties': False,
        'properties': {
            'region': {'type': 'string', 'enum': ['dram', 'scratchpad']},
            'offset': {'type': 'integer'},
            'size': {'type': 'integer'},
            'kind': {'type': 'string',
                     'enum': ['firmware_runtime', 'trusted_device_state']},
            'name': {'type': 'string'},
            'reason': {'type': 'string'},
        },
        'required': ['region', 'offset', 'size', 'kind', 'name', 'reason'],
    }
    return {
        'type': 'object', 'additionalProperties': False,
        'properties': {
            'schema': {'type': 'integer', 'enum': [1]},
            'edits': {'type': 'array', 'items': edit},
            'function_audits': {'type': 'array', 'items': audit},
            'memory_exceptions': {'type': 'array', 'items': memory_exception},
            'reason': {'type': 'string'},
        },
        'required': ['schema', 'edits', 'function_audits', 'memory_exceptions', 'reason'],
    }


def send_request(payload: dict, api_key: str) -> dict:
    body = json.dumps(payload, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    outgoing = request.Request(
        RESPONSE_URL, data=body, method='POST',
        headers={'Authorization': 'Bearer ' + api_key,
                 'Content-Type': 'application/json'},
    )
    with request.urlopen(outgoing, timeout=TIMEOUT_SECONDS) as stream:
        raw = stream.read(MAX_RESPONSE_BYTES + 1)
    if len(raw) > MAX_RESPONSE_BYTES:
        raise ValueError('Model API response exceeds 1 MiB')
    return json.loads(raw.decode('utf-8'))


def response_text(response: dict) -> str | None:
    if not isinstance(response, dict) or response.get('status') != 'completed':
        return None
    texts = []
    output = response.get('output')
    if not isinstance(output, list):
        return None
    for item in output:
        if not isinstance(item, dict):
            return None
        if item.get('type') == 'reasoning':
            continue
        if item.get('type') != 'message':
            return None
        contents = item.get('content')
        if not isinstance(contents, list):
            return None
        for content in contents:
            if not isinstance(content, dict):
                return None
            if content.get('type') != 'output_text' or not isinstance(content.get('text'), str):
                return None
            texts.append(content['text'])
    return texts[0] if len(texts) == 1 else None


def response_usage(response: dict) -> dict | None:
    if not isinstance(response, dict):
        return None
    usage = response.get('usage')
    if not isinstance(usage, dict):
        return None
    details = usage.get('input_tokens_details') or {}
    output_details = usage.get('output_tokens_details') or {}
    if not isinstance(details, dict) or not isinstance(output_details, dict):
        return None
    input_tokens = usage.get('input_tokens')
    output_tokens = usage.get('output_tokens')
    cached = details.get('cached_tokens', 0)
    reasoning = output_details.get('reasoning_tokens', 0)
    values = (input_tokens, output_tokens, cached, reasoning)
    if (any(type(value) is not int or value < 0 for value in values) or
            cached > input_tokens or reasoning > output_tokens):
        return None
    return {'input_tokens': input_tokens, 'output_tokens': output_tokens,
            'cached_input_tokens': cached, 'reasoning_output_tokens': reasoning,
            'total_tokens': input_tokens + output_tokens}


def execute(root: Path, config: dict, packet_path: Path, *, transport=send_request) -> dict:
    api_key = os.environ.get('OPENAI_API_KEY')
    model = config['lockstep'].get('repair_model')
    if not api_key:
        return {'status': 'attention_required', 'reason': 'model_api_key_missing',
                'model_invocations': 0}
    if not isinstance(model, str) or not model:
        return {'status': 'attention_required', 'reason': 'model_api_model_missing',
                'model_invocations': 0}
    source_roots = tuple(Path(value) for value in config['lockstep'].get('source_roots', ['src']))
    before = source_snapshot(root, source_roots)
    packet = json.loads(packet_path.read_text(encoding='utf-8'))
    if before['identity'] != packet['identities']['native_source_identity']:
        raise RuntimeError('Source changed before model repair request')
    shared = packet['identities'].get('shared_source_identity')
    if shared is not None and shared != shared_source_identity(root, config):
        return {'status': 'attention_required', 'reason': 'shared_source_changed',
                'model_invocations': 0}
    try:
        prompt = build_prompt(packet, root=root, source_roots=source_roots)
    except ValueError:
        return {'status': 'attention_required', 'reason': 'incomplete_model_context',
                'model_invocations': 0}
    payload = {
        'model': model, 'input': prompt, 'store': False,
        'tools': [], 'parallel_tool_calls': False,
        'max_output_tokens': int(config['lockstep'].get('repair_max_output_tokens', 16384)),
        'text': {'format': {'type': 'json_schema', 'name': 'lockstep_repair',
                            'strict': True, 'schema': proposal_schema()}},
    }
    try:
        response = transport(payload, api_key)
    except (OSError, ValueError) as error:
        return {'status': 'attention_required', 'reason': 'model_api_transport_error',
                'error': str(error), 'model_invocations': 1}
    if source_snapshot(root, source_roots)['identity'] != before['identity']:
        return {'status': 'attention_required', 'reason': 'model_modified_source',
                'model_invocations': 1}
    if shared is not None and shared != shared_source_identity(root, config):
        return {'status': 'attention_required', 'reason': 'shared_source_changed',
                'model_invocations': 1}
    accounting = {'model_invocations': 1}
    usage = response_usage(response)
    if usage is not None:
        accounting['model_usage'] = usage
    text = response_text(response)
    if text is None:
        return {'status': 'attention_required', 'reason': 'model_api_incomplete',
                **accounting}
    try:
        proposal = json.loads(text)
    except json.JSONDecodeError:
        return {'status': 'attention_required', 'reason': 'invalid_model_proposal',
                **accounting}
    if not isinstance(proposal, dict) or not proposal.get('edits'):
        return {'status': 'attention_required',
                'reason': proposal.get('reason', 'model_proposed_no_edits')
                if isinstance(proposal, dict) else 'invalid_model_proposal',
                **accounting}
    return {'status': 'proposed', 'proposal': proposal, **accounting}
