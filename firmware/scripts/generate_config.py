from pathlib import Path

Import("env")


def read_env(path):
    values = {}
    if not path.exists():
        raise RuntimeError(f"Missing firmware environment file: {path}")
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")
    return values


project_dir = Path(env.subst("$PROJECT_DIR"))
config_env = read_env(project_dir / ".env")
required = [
    "WIFI_SSID",
    "WIFI_PASSWORD",
    "DTRDESK_API_URL",
    "DTRDESK_SYNC_URL",
    "DTRDESK_DEVICE_ID",
    "DTRDESK_DEVICE_KEY",
    "DTRDESK_FIRMWARE_VERSION",
]
missing = [key for key in required if not config_env.get(key)]
if missing:
    raise RuntimeError(f"Missing firmware environment values: {', '.join(missing)}")

output = project_dir / "src" / "build_config.h"
lines = ["// Generated from firmware/.env. Do not edit or commit.", "#ifndef BUILD_CONFIG_H", "#define BUILD_CONFIG_H"]
for key in required:
    lines.append(f'#define {key} "{config_env[key]}"')
lines.extend(["#endif", ""])
output.write_text("\n".join(lines))