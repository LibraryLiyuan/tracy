# Vendored YAML parser

This directory contains the pure-Python modules from PyYAML 6.0.2.

- Runtime import path: `vendor/yaml`
- Loader used by the Skill: `yaml.safe_load_all`
- Native `_yaml` extension: intentionally omitted
- Runtime package installation and network access: forbidden
- License: `PyYAML-LICENSE.txt`

The copy is pinned so every machine resolves the same authoring YAML before
the normalized JSON object is validated by `tracy-query`.
