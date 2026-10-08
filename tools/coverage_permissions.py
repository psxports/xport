"""Resolve optional permissions from the selected consuming project"""


def coverage_permissions(project):
    defaults = {
        "allow_unverified_translation": False,
        "allow_missing_bios_stubs": False,
        "allow_missing_sdk_stubs": False,
    }
    value = project.get("coverage_permissions", {})
    if not isinstance(value, dict) or set(value) - set(defaults):
        raise ValueError("Invalid project coverage_permissions")
    if any(type(item) is not bool for item in value.values()):
        raise ValueError("Project coverage permissions must be booleans")
    return {**defaults, **value}
