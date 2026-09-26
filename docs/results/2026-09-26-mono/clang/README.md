# Nine-file Clang comparison

This September 26, 2026 check used Clang 19.1.5 `-O2` on Windows x64, logical
CPU 0, seven trials, and minimum 30 ms batches. Six actual mono files and three
stereo files produced 441 successful trials. Clang uses the generic decoder;
the MSVC-only helpers are excluded during preprocessing.

Before and current identify the same source revisions as the
[full MSVC dataset](../README.md). See [performance and quality](../../../performance.md)
and [benchmark instructions](../../../benchmarks.md) for context and reproduction.

| Channels | Quality | Assets | Before ms | Current ms | Change in time |
| --- | --- | ---: | ---: | ---: | ---: |
| Mono | High | 6 | 9.6993 | 9.7019 | +0.03% |
| Mono | Medium | 6 | 9.8044 | 9.7982 | -0.06% |
| Mono | Low | 6 | 9.7649 | 9.7829 | +0.18% |
| Stereo | High | 3 | 58.0526 | 57.6219 | -0.74% |
| Stereo | Medium | 3 | 58.2298 | 57.8156 | -0.71% |
| Stereo | Low | 3 | 57.6733 | 57.7826 | +0.19% |

Times sum per-file median batch means. Every change is below 1%; these results
do not establish a Clang speed improvement or represent the full corpus.
QOA took 30.7776 ms for the mono subset and 187.1395 ms for the stereo subset.
Ratios describe these compiled implementations, not an inherent format ranking.

All PCM and encoded fingerprints match the frozen corpus, all case/trial IDs
are present, and summaries reconcile with raw trials. Fingerprints are
consistency checks, not cryptographic PCM byte comparisons. Metadata retains
compiler/version/flags and source/executable hashes.

[Raw trials](raw.jsonl.gz), [metadata](metadata.json),
[per-file results](per_file.csv), [comparisons](comparisons.csv),
[totals](totals.csv), [channel totals](channels.csv), and
[data validation](validation.json).
