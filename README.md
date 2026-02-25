# Dynamic Memory Allocator (Resume Version)

This directory is a standalone version of my allocator implementation, stripped
to code I can publish publicly while preserving the core allocator strategy.

## Clean Demo Command

```bash
TRACE_PATTERN='demo-example.rep' ./benchmark.sh
```

## What It Demonstrates

- Segregated free lists with 6 bins.
- 16-byte payload alignment.
- Header bit-packing:
  - bit 0: allocation state
  - bits 1-3: bin index
  - upper bits: payload size
- First-fit lookup in the selected bin, then fallback to larger bins.
- Heap extension with geometric chunk growth.
- Address-ordered free-list insertion.
- Forward/backward coalescing via physical adjacency checks.
- Split heuristic that prefers splitting when:
  - the next adjacent block is free, or
  - the remainder is both large enough and meaningful.

## Diagram (ASCII)

```text
free_heads[0..5]
  bin0 -> [H:size<=512, free] -> [H, free] -> NULL
  bin1 -> [H:size<=2048, free] -> NULL
  ...
  bin5 -> [H:large, free] -> NULL

Heap region (address-ordered blocks):

low addr
  +-------------------------+
  | Header A (allocated)    |  metadata: [size | bin | alloc=1]
  +-------------------------+
  | Payload A               |
  +-------------------------+
  | Header B (free)         |  metadata: [size | bin | alloc=0]
  +-------------------------+
  | Payload B               |
  +-------------------------+
  | Sentinel Header         |  size=0, alloc=1, next=0xFEEDFACE
  +-------------------------+
high addr
```

## Design Tradeoffs

- Segregated bins reduce search time, but can increase fragmentation when a
  request just misses a bin boundary.
- First-fit is fast and simple, but can leave suboptimal holes compared to
  best-fit.
- Address-ordered free lists make coalescing straightforward, but insertion is
  O(n) within a bin.
- Split heuristic avoids tiny unusable fragments, but sometimes keeps a larger
  block than strictly necessary, trading memory efficiency for fewer list ops.
- Geometric heap growth lowers allocator call overhead, but can over-reserve
  memory for bursty workloads.

## Benchmark

Run:

```bash
./benchmark.sh
```

Output:

- `benchmark_results.csv` (raw benchmark data)

The report includes `avg_utilization`, computed as the average over all
instructions of:

`allocated_space / heap_size`

For `umalloc`, heap size comes from allocator `extend` growth (total managed
payload across regions), so utilization is strategy-sensitive.
For `libc_malloc`, this field is `nan` because allocator heap internals are not
available to this benchmark harness.

`benchmark.sh` runs all `*.rep` files in:

- `./testcases/`

If your traces are in a different location, override the directory:

```bash
TESTCASE_DIR=/path/to/traces ./benchmark.sh
```

Run only the custom demo testcase:

```bash
TRACE_PATTERN='demo-example.rep' ./benchmark.sh
```

Curated testcase set moved into `./testcases/`:

- Demo (tracked): `demo-example.rep`
- Sanity: `short1.rep`, `short1-bal.rep`, `short2.rep`, `short2-bal.rep`
- Real-program: `cccp.rep`, `cccp-bal.rep`, `cp-decl.rep`, `cp-decl-bal.rep`, `amptjp.rep`, `amptjp-bal.rep`, `expr.rep`, `expr-bal.rep`
- Coalescing/fragmentation: `coalescing.rep`, `coalescing-bal.rep`, `binary.rep`, `binary-bal.rep`, `binary2.rep`, `binary2-bal.rep`
- Random stress: `random.rep`, `random-bal.rep`, `random2.rep`, `random2-bal.rep`

## Files

- `umalloc.h`: public allocator interface and header layout.
- `umalloc.c`: core allocator implementation.
- `benchmark.c`: allocator benchmark harness.
- `benchmark.sh`: benchmark runner + results generator.
- `benchmark_results.csv`: latest benchmark results.
- `testcases/`: curated trace suite (ignored via `.gitignore` except `demo-example.rep`).
- `Makefile`: build helper for benchmark target.
