# Architecture and tradeoffs

## Batch contract

`Batch::physical_size` counts initialized rows in every column. `selection.size()`
counts active rows, and each selection entry is an index below `physical_size`.
Selections can be implicit identity ranges or contiguous `uint16_t` indices.
The configurable execution capacity is 1–65,535 rows (default 2,048). Empty-schema
batches still carry a row count, which is necessary for pruned `COUNT(*)` plans.

Columns hold contiguous int64, double, byte-valued booleans, or uint32 offsets
into a contiguous string arena. Validity uses one bit per row, with one meaning
valid. NULL payload bytes have no semantic meaning. Reset retains buffer
capacity. Strings may include embedded NUL bytes. A string arena is limited to
4 GiB; execution row counts and blocking payload row IDs are separate concepts.

Every output batch owns its values. Scan and projection materialize output;
filter changes only the selection. This costs copying but makes lifetime rules
explicit: consumers can retain a batch by copying it, and producers can reuse
their buffers on the next call. `Evaluator::evaluate` returns a borrowed column
reference valid until input mutation or the next evaluation. Column-reference
expressions return the input vector directly; computed nodes reuse scratch.

## Execution

The pull interface calls `next(Batch&)` once per batch. It makes operator
composition, early LIMIT termination, and continuation through duplicate join
chains easy to inspect. It avoids virtual dispatch per tuple at normal batch
sizes. It is single-threaded and does not fuse expressions across operators.
Push pipelines could reduce intermediate materialization and coordinate
parallelism; they would require scheduler and pipeline lifetime machinery not
needed for this implementation.

Expressions are compiled to postorder nodes with bound column positions and
reusable vectors. Each node evaluates over a selection, rather than interpreting
an AST for each row. Scalar NULL-aware loops are the general path. Dense,
all-valid int64 `< constant` and double addition use isolated scalar kernels.

## Hashing and blocking operators

The shared hash index uses 16-byte buckets containing a 64-bit hash and a payload
row ID. It starts at 16 buckets, maintains at most 70% occupancy, resolves
collisions by linear probing with full key equality checks, and doubles and
rehashes on growth. Payload is separate from the index to keep unsuccessful
probes from touching aggregate state or joined rows. Hashes are deterministic;
this is not a hash-flood-resistant service boundary.

Aggregation stores composite keys in columnar arrays and aggregate states in a
contiguous group-major array. State consists of numeric accumulators and seen/
overflow flags. New keys can grow buffers; existing groups require no per-row
heap allocations. SQL NULL grouping, COUNT(*), COUNT(column), SUM, MIN and MAX
are supported. MIN/MAX and SUM accept numeric inputs; grouping accepts all
supported types. Empty global aggregation yields one row; empty grouped
aggregation yields none.

The inner join stores build payload in columns and duplicate links in contiguous
row-index arrays. NULL or NaN keys never enter the build index. Probe heads are
found for an entire input batch before duplicate chains are expanded. The
continuation records the probe row and duplicate link, so a single key can
produce more than one output batch. Switching build sides preserves the public
left-then-right schema, but does not promise row order.

Sort materializes column payload, stable-sorts an array of row IDs, then emits
batches. LIMIT is streaming; a LIMIT above sort still requires a full sort.
All blocking state is in memory: no spilling, memory quota, parallel partitioned
join, or external sorting. Allocation failure propagates as an exception.

## Numeric and NULL semantics

Boolean logic uses SQL's three truth values. WHERE retains only valid true.
Comparisons with NULL yield NULL. Integer arithmetic is checked; overflow and
division by zero yield NULL. Integer division truncates toward zero. An int64
SUM becomes NULL if any intermediate accumulator overflows. COUNT overflow
throws. Double arithmetic uses IEEE values except division by either signed
zero yields NULL. No implicit numeric casts are performed.

Comparisons retain IEEE NaN behavior. For grouping, NaNs form one group and
positive/negative zero share a group. NaN never matches an equality join key.
Sort and numeric MIN/MAX use an order where NaN follows other non-NULL doubles;
NULL placement in sort is independent of ascending/descending order. SUM follows
input order and is not compensated, so different valid join orders can change
floating-point rounding. Strings use bytewise lexicographic comparisons with no
locale or collation processing.

