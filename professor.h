#ifndef PROFESSOR_H
// professor.h - a single file profiling library

// Most common
// - trigger gets hit multiple times, in uncertain hierarchy
// - init each var

#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#include <intrin.h> // __rdtsc()

// TODO: for DLLs prof_set_global_state
// TODO: atomics
// TODO: multiple threads
// TODO: make skippable without having to find closing tag
// TODO: combine hashmap array with normal dynamic array (hash -> index)

// NOTE: if not defined, these are not atomic!
#ifndef prof_atomic_add
#define prof_atomic_add(a, b) (*(a) += (b))
#endif

#ifndef prof_atomic_exchange
static inline uint64_t
prof_atomic_exchange(uint64_t *a, uint64_t b)
{
    uint64_t result = *a;
    *a = b;
    return result;
}
#endif

typedef uint32_t ProfIdx;

// TODO: rolling buffer of multiple frames
typedef struct ProfRecord {
	char const *name;
	char const *filename;

	uint32_t line_num;
	// record type? range/marker/thread/function/...

    // could calculate this from the record tree...
	uint64_t hits_n__cycles_n; // Top half hits_n, bottom half cycles_n
} ProfRecord;

typedef struct ProfRecordSmpl {
    ProfIdx  record_i;
    ProfIdx  parent_i; // if parent_i == record_i, is root
    uint64_t cycles_start;
    uint64_t cycles_end; // 0xFFFFFFF... if not finished
    // thread id/proc id?
} ProfRecordSmpl;

typedef struct Prof {
    ProfRecord *records; // dynamic array
    ProfIdx     records_n;
    ProfIdx     records_m;

    ProfRecordSmpl *record_smpl_tree; // dynamic
    ProfIdx     record_smpl_tree_n;
    ProfIdx     record_smpl_tree_m;
    ProfIdx open_record_smpl_tree_i; // the deepest record that is still open (check if this record is closed to see if all are closed)

    uint64_t freq;

    // NOTE: this is needed so that different allocators aren't used across dll boundaries
    void *(*reallocate)(void *allocator, void *ptr, size_t size);
    void *allocator;
} Prof;

// TODO: preprocessor gate
#include <stdlib.h>
static void *
prof_realloc(void *allocator, void *ptr, size_t size)
{
    (void)allocator;
    return realloc(ptr, size);
}

static inline void *
prof_grow(Prof *prof, void *ptr, ProfIdx *max, size_t elem_size)
{
    // TODO: make threadsafe

    if (! prof->reallocate)
    {   prof->reallocate = prof_realloc;   }

    *max = (*max
            ? *max * 2
            : 64);
    size_t size = *max * elem_size;
    void *result = prof->reallocate(prof->allocator, ptr, size);
    return result;
}

static inline ProfIdx
prof_top_record_i(Prof *prof)
{
    // TODO: thread id...
    ProfIdx result = ((prof->record_smpl_tree_n &&
                       ~prof->open_record_smpl_tree_i)
                      ? prof->record_smpl_tree[prof->open_record_smpl_tree_i].record_i
                      : ~(ProfIdx) 0);
    return result;
}

static inline ProfIdx
prof_new_record(Prof *prof, char const *name, char const *filename, uint32_t line_num)
{
    if (prof->records_n == prof->records_m)
    {   prof->records = (ProfRecord *)prof_grow(prof, prof->records, &prof->records_m, sizeof(*prof->records));   }

    ProfRecord record = {0}; {
        record.name     = name;
        record.filename = filename;
        record.line_num = line_num;
    }
    ProfIdx result        = prof->records_n++;
    prof->records[result] = record;

    return result;
}

static inline void
prof_start_(Prof *prof, ProfIdx record_i)
{
    uint64_t cycles_start = __rdtsc();
    ProfIdx  parent_i = (~ prof->open_record_smpl_tree_i
                         ? prof->open_record_smpl_tree_i
                         : prof->record_smpl_tree_n);

    if (prof->record_smpl_tree_n == prof->record_smpl_tree_m)
    {   prof->record_smpl_tree = (ProfRecordSmpl *)prof_grow(prof, prof->record_smpl_tree, &prof->record_smpl_tree_m, sizeof(*prof->record_smpl_tree));   }

    ProfRecordSmpl record_smpl; {
        record_smpl.record_i     = record_i;
        record_smpl.parent_i     = parent_i;
        record_smpl.cycles_start = cycles_start;
        record_smpl.cycles_end   = ~(uint64_t) 0;
    };

    prof->open_record_smpl_tree_i = prof->record_smpl_tree_n++;
    prof->record_smpl_tree[prof->open_record_smpl_tree_i] = record_smpl;
}

static inline void
prof_mark_(Prof *prof, ProfIdx record_i)
{
    uint64_t cycles = __rdtsc();
    ProfIdx  parent_i = prof->open_record_smpl_tree_i;

    if (prof->record_smpl_tree_n == prof->record_smpl_tree_m)
    {   prof->record_smpl_tree = (ProfRecordSmpl *)prof_grow(prof, prof->record_smpl_tree, &prof->record_smpl_tree_m, sizeof(*prof->record_smpl_tree));   }

    ProfRecordSmpl record_smpl; {
        record_smpl.record_i     = record_i;
        record_smpl.parent_i     = parent_i;
        record_smpl.cycles_start = cycles;
        record_smpl.cycles_end   = cycles;
    };

    prof->record_smpl_tree[prof->record_smpl_tree_n++] = record_smpl;
}

#define PROF_CAT1(a,b) a ## b
#define PROF_CAT2(a,b) PROF_CAT1(a,b)
#define PROF_CAT(a,b)  PROF_CAT2(a,b)
#define prof_static_local_record_i_ PROF_CAT(prof_static_local_record_i_, __LINE__)
#define prof_scope_once PROF_CAT(prof_scope_once, __LINE__)

#define PROF_NEW_RECORD(prof, name) \
    static ProfIdx prof_static_local_record_i_ = ~(ProfIdx) 0; \
    if (! ~prof_static_local_record_i_) /* TODO: atomic */ \
    {   prof_static_local_record_i_ = prof_new_record(prof, name, __FILE__, __LINE__);   } 

#define prof_start(prof, name) \
    do { \
        PROF_NEW_RECORD(prof, name) \
        prof_start_(prof, prof_static_local_record_i_); \
    } while (0)

#define prof_mark(prof, name) \
    do { \
        PROF_NEW_RECORD(prof, name) \
        prof_mark_(prof, prof_static_local_record_i_); \
    } while (0)


// NOTE: can't nest without braces
#define prof_scope(prof, name) prof_scope_n(prof, name, 1)
#define prof_scope_n(prof, name, n) prof_start(prof, name); \
    for (int prof_scope_once = 0; prof_scope_once++ == 0; prof_end_n(prof, n))

// returns the index of the record referenced, so you can double check this is correct
static inline ProfIdx
prof_end_n(Prof *prof, uint32_t hits_n)
{
    assert(prof->record_smpl_tree   &&
           prof->record_smpl_tree_n &&
           "no record samples taken at all - nothing to close");
    assert(~prof->open_record_smpl_tree_i &&
           "no open prof records - you've already closed them all. Mismatched start and end records?");

    ProfRecordSmpl *record_smpl      = &prof->record_smpl_tree[prof->open_record_smpl_tree_i];
    uint64_t        cycles_end       = __rdtsc();
    uint64_t        cycles_n         = cycles_end - record_smpl->cycles_start;
    uint64_t        hits_n__cycles_n = (uint64_t)cycles_n | ((uint64_t)hits_n << 32);

    record_smpl->cycles_end = cycles_end;
    prof_atomic_add(&prof->records[record_smpl->record_i].hits_n__cycles_n, hits_n__cycles_n); // TODO: this could be done after the fact

    int is_tree_root = prof->open_record_smpl_tree_i == record_smpl->parent_i;
    prof->open_record_smpl_tree_i = (! is_tree_root
                                     ? record_smpl->parent_i
                                     : ~(ProfIdx) 0);

    return record_smpl->record_i;
}

static inline ProfIdx
prof_end(Prof *prof)
{   return prof_end_n(prof, 1);   }

#if 1 // OUTPUT

static inline void
prof_record_read_clear(Prof *prof, ProfIdx record_i, uint32_t *hits_n, uint32_t *cycles_n)
{
    uint64_t hits_n__cycles_n = prof->records[record_i].hits_n__cycles_n;
    if (hits_n)   { *hits_n   = (uint32_t)(hits_n__cycles_n >> 32); }
    if (cycles_n) { *cycles_n = (uint32_t)(hits_n__cycles_n & 0xFFFFFFFF); }
}

static void
prof_dump_timings_file(FILE *out, Prof *prof, int is_first_dump)
{
    double ms = (prof->freq
                 ? prof->freq / 1000.0
                 : 1.0);
    ProfRecord *records   = prof->records;
    ProfIdx     records_n = prof->records_n;
    for (ProfIdx record_i = 0; record_i < records_n; ++record_i)
    {
        /* ProfRecord record   = records[record_i]; */
        uint32_t   hits_n   = 0,
                   cycles_n = 0;
        prof_record_read_clear(prof, record_i, &hits_n, &cycles_n); // TODO: something
    }

    if (! is_first_dump)
    {   fputs(",\n\n", out);   }

    ProfRecordSmpl *record_smpl_tree   = prof->record_smpl_tree;
    size_t          record_smpl_tree_n = prof->record_smpl_tree_n;
    for (size_t record_smpl_tree_i = 0; record_smpl_tree_i < record_smpl_tree_n; ++record_smpl_tree_i)
    {
        // TODO: units
        ProfRecordSmpl record_smpl = record_smpl_tree[record_smpl_tree_i];
        ProfRecord     record      = records[record_smpl.record_i];
        if (record_smpl_tree_i > 0)
        {   fprintf(out, ",\n");   }

        if (record_smpl.cycles_start != record_smpl.cycles_end)
        { // normal record
            fprintf(out, "    {"
                    "\"name\":\"%s\", "
                    "\"ph\":\"X\", "
                    "\"ts\": %lf, "
                    "\"dur\": %lf, "
                    "\"pid\": 0, "
                    "\"tid\": 0"
                    "}",
                    /* record.filename, */
                    record.name,
                    record_smpl.cycles_start / ms,
                    (record_smpl.cycles_end - record_smpl.cycles_start) / ms
            );
        }

        else
        { // mark
            fprintf(out, "    {"
                    "\"name\":\"%s\", "
                    "\"ph\":\"i\", "
                    "\"ts\": %lf, "
                    "\"pid\": 0, "
                    "\"tid\": 0"
                    "}",
                    /* record.filename, */
                    record.name,
                    record_smpl.cycles_start / ms
            );
        }
    }
    fflush(out);

    prof->record_smpl_tree_n = 0;
}

static inline FILE *
prof_dump_timings_init(char const *filename)
{
    FILE *file = fopen(filename, "w");
    fputs("[\n", file);
    return file;
}

#endif // OUTPUT

#if 1 // INVARIANTS

#include <string.h>

static inline int
prof_invar_unique_records(Prof *prof)
{
    int result = 1;
    ProfRecord *records = prof->records;

    for (ProfIdx record_i = 0; record_i < prof->records_n; ++record_i)
    {
        ProfRecord record = records[record_i];

        for (ProfIdx record_j = record_i + 1; record_j < prof->records_n; ++record_j)
        {
            ProfRecord test_record = records[record_j];

            if (record.line_num == test_record.line_num &&
                ! strcmp(record.name, test_record.name) &&
                ! strcmp(record.filename, test_record.filename))
            {   result = 0; break;   }
        }
    }

    return result;
}

#endif // INVARIANTS

#define PROFESSOR_H
#endif//PROFESSOR_H
