#pragma once

#include "mutation_reader.hh"

namespace enormous_table {

extern logging::logger elogger;

class enormous_table_reader final : public flat_mutation_reader::impl {
public:
    static constexpr uint64_t CLUSTERING_ROW_COUNT = 4500ULL * 1000ULL * 1000ULL;

    enormous_table_reader(schema_ptr, const dht::partition_range&, const query::partition_slice&);

    virtual ~enormous_table_reader();

    virtual future<> fill_buffer(db::timeout_clock::time_point) override;
    virtual void next_partition() override;
    virtual future<> fast_forward_to(const dht::partition_range&, db::timeout_clock::time_point) override;
    virtual future<> fast_forward_to(position_range, db::timeout_clock::time_point) override;

private:
    void get_next_partition();
    void do_fast_forward_to(const dht::partition_range&);

    partition_key get_pk();
    dht::decorated_key get_dk() {
        return dht::decorate_key(*_schema, get_pk());
    }

    enum class partition_production_state {
        not_started,
        before_partition_start,
        after_partition_start,
        before_partition_end,
        after_partition_end,
    };

    schema_ptr _schema;
    const query::partition_slice& _slice;
    streamed_mutation::forwarding _fwd;
    partition_production_state _pps = partition_production_state::not_started;

    bool _partition_in_range = false;
    uint64_t _clustering_row_idx = 0;
};

struct virtual_reader {
    flat_mutation_reader operator()(schema_ptr schema,
            reader_permit,
            const dht::partition_range& range,
            const query::partition_slice& slice,
            const io_priority_class& pc,
            tracing::trace_state_ptr trace_state,
            streamed_mutation::forwarding fwd,
            mutation_reader::forwarding fwd_mr) {
        auto reader = make_flat_mutation_reader<enormous_table_reader>(schema, range, slice);
        if (fwd == streamed_mutation::forwarding::yes) {
            // elogger.info("make forwardable");
            return make_forwardable(std::move(reader));
        }
        // elogger.info("make non-forwardable");
        return reader;
    }
};

}
