# Analysis Notes — Reading Order

Forensic reverse-engineering notes from the research phase (MediaTek MT5862 TV binaries).
Historical reference only; the frozen downmix contract lives in
`../docs/MATHEMATICAL_CONTRACT.md`.

## Phase reports (read in order)

| # | File | Topic |
|---|------|-------|
| 1 | `PHASE_1_FINDINGS.md` | Forensic extraction — file inventory |
| 2 | `PHASE_2_AUDIO_PATH.md` | Verified audio execution graph |
| 3 | `PHASE_3_MINIMAL_SET.md` | Minimal working-set classification |
| 4 | `PHASE_4_DECODER_RECONSTRUCTION.md` | Old decoder reconstruction |

## Topical investigations

| File | Topic |
|------|-------|
| `PHASE_DOWNMIX_INVESTIGATION.md` | Proof that downmix happens inside the decoder library |
| `PHASE_CODEC_SELECTION.md` | How the software decoder wins over hardware |
| `OMX_REGISTRATION_MECHANISM.md` | OMX software component registration (Android 9) |
| `GOLDEN_AUDIO_REFERENCE.md` | Golden reference coefficients and behavior |
| `FINAL_ARCHITECTURE_CANDIDATES.md` | Architecture candidates comparison |
| `IMPLEMENTATION_PLAN.md` | Phased implementation plan |

## Tooling

| File | Purpose |
|------|---------|
| `parse_elf.py` | ELF/dynamic-section string analysis helper |
