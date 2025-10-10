# Benchmark Report Generation Information

**Generated:** Fri Oct 10 04:34:28 PM CDT 2025
**Hostname:** smc300x-clt-r4c7-26
**Kernel:** 5.15.0-143-generic
**CPU:** AMD EPYC 9354 32-Core Processor
**Platform:** Bare Metal (not virtualized)

## Benchmark Configuration
- **Runs per scenario:** 10
- **Scenarios:** 6 (0μs, 5μs, 50μs, 100μs, 500μs, 1000μs)
- **Methods:** Baseline, LTTng, eBPF
- **Total tests:** 180 (6 scenarios × 3 methods × 10 runs)

## Environment Details
Linux smc300x-clt-r4c7-26 5.15.0-143-generic #153-Ubuntu SMP Fri Jun 13 19:10:45 UTC 2025 x86_64 x86_64 x86_64 GNU/Linux

## Key Performance Results
- eBPF overhead for 100μs function: ~8-10μs (8-10%)
- eBPF overhead for 500μs+ functions: <1%
- Results are representative of production bare metal performance
