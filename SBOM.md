# Source SBOM

Source repository inventory only; completeness is UNKNOWN. Syft inventories tracked source/package manifests. Gitlinks and declared vendored/model evidence are inspected at immutable pins. CMake declarations are conditional requirements, not resolved installations. This is NOT proof of operating-system packages, static-link closure, build-time resolution, or the contents of release binaries.

Generation: **success**

## Source identity

wave-owl/fldigi-rtty-core `4e5cbe77e48426df6cdee6b883dbd4fd7c105c69`

- sbom:role: source-root
- sbom:git-commit: 4e5cbe77e48426df6cdee6b883dbd4fd7c105c69
- sbom:git-tree: f955140d05c7391479dffa654ce830a6873b1a9b
- sbom:source-repository: https://github.com/wave-owl/fldigi-rtty-core
- sbom:release-tags: ["v0.2.0"]
- sbom:license-evidence: [{"path": "LICENSE", "sha256": "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986"}]
- sbom:license-resolution: unresolved; manual review required

## Generation errors

None.

## Components and unresolved licenses

| Component | Version | Role | License |
| --- | --- | --- | --- |
| wave-owl/fldigi-rtty-core | 4e5cbe77e48426df6cdee6b883dbd4fd7c105c69 | source-root | unresolved; manual review required |
| src/rtty_rx.c | e7c3ab3709ad62a7d91829364eb48a05fa325c0d | declared-vendored-source | unresolved; manual review required |
| src/rtty_tx.c | e7c3ab3709ad62a7d91829364eb48a05fa325c0d | declared-vendored-source | unresolved; manual review required |
| actions/checkout | v4 | Syft source/package discovery | unresolved |
| tracked-source/.github/workflows/ci.yml | unresolved | Syft source/package discovery | unresolved |

License evidence paths/hashes, source manifests, model hashes and conditional
CMake declarations are recorded in SBOM.cdx.json. Declared upstream pins do not
prove vendored equivalence. No dependency installation or build was performed.
