## Description

<!-- What changed, and why. The diff shows how; this should explain the motivation. -->

## Type of Change

<!-- Check the relevant option(s) -->

- [ ] Bug fix
- [ ] New feature
- [ ] Breaking change (language surface, `.kbc` format, or CLI)
- [ ] Documentation update
- [ ] Refactoring
- [ ] Test addition

## Backends

<!-- Khudra runs the same program three ways, and their output must be byte-identical.
     Check the ones you exercised. -->

- [ ] Bytecode VM (`khudra run`)
- [ ] Native, in process (`khudra run --native`)
- [ ] Native, standalone (`khudra build`)
- [ ] Not applicable (front end, docs, or build only)

## Testing

<!-- Paste the commands you ran and what they reported. -->

- [ ] `ctest --test-dir build --output-on-failure` — full suite green
- [ ] `ctest --test-dir build-asan --output-on-failure` — green under `-DKHU_SANITIZE=ON`
- [ ] `python3 scripts/validate_structure.py` and `python3 scripts/detect_artifacts.py`
- [ ] New tests added (if applicable)

## Checklist

- [ ] Rebased on the latest `upstream/dev`, and targeting `dev`
- [ ] Code follows the conventions in [CONTRIBUTING.md](../CONTRIBUTING.md)
- [ ] Self-reviewed the diff
- [ ] No new compiler warnings
- [ ] Documentation updated (which file under `docs/`?)
- [ ] Commit messages follow the project style — `(type) Description`
- [ ] No build artifacts, generated files, secrets, or absolute local paths committed

## Invariants

<!-- Only if your change touches these. See CONTRIBUTING.md section 3. -->

- [ ] Materialization still goes through `khu_proc_materialize`, in the order in `docs/procedures.md`
- [ ] Object layout still matches `src/vm/object.h`
- [ ] Output is byte-identical across all three backends — stdout, stderr, and exit code
- [ ] Not applicable

## Related Issues

<!-- Link any related issues -->

Closes #

## Terminal Output

<!-- For anything a user would see: program output, diagnostics, trap text, CLI behaviour.
     Paste it in a code block rather than describing it. -->
