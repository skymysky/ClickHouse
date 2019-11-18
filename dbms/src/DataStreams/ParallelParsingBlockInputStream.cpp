#include <DataStreams/ParallelParsingBlockInputStream.h>
#include "ParallelParsingBlockInputStream.h"

namespace DB
{

void ParallelParsingBlockInputStream::segmentatorThreadFunction()
{
    setThreadName("Segmentator");
    try
    {
        while (!finished)
        {
            const auto current_unit_number = segmentator_ticket_number % processing_units.size();
            auto & unit = processing_units[current_unit_number];

            {
                std::unique_lock lock(mutex);
                segmentator_condvar.wait(lock,
                    [&]{ return unit.status == READY_TO_INSERT || finished; });
            }

            if (finished)
            {
                break;
            }

            assert(unit.status == READY_TO_INSERT);

            // Segmentating the original input.
            unit.segment.resize(0);

            const bool have_more_data = file_segmentation_engine(original_buffer,
                unit.segment, min_chunk_size);

            // Creating buffer from the segment of data.
            auto new_buffer = BufferBase::Buffer(unit.segment.data(),
                                                 unit.segment.data() + unit.segment.size());

            unit.readbuffer->buffer().swap(new_buffer);
            unit.readbuffer->position() = unit.readbuffer->buffer().begin();

            unit.parser = std::make_unique<InputStreamFromInputFormat>(
                    input_processor_creator(*unit.readbuffer, header, context, row_input_format_params, format_settings)
            );

            if (!have_more_data)
            {
                unit.is_last = true;
            }

            unit.status = READY_TO_PARSE;
            scheduleParserThreadForUnitWithNumber(current_unit_number);
            ++segmentator_ticket_number;

            if (!have_more_data)
            {
                break;
            }
        }
    }
    catch (...)
    {
        onBackgroundException();
    }
}

void ParallelParsingBlockInputStream::parserThreadFunction(size_t current_unit_number)
{
    try
    {
        setThreadName("ChunkParser");

        auto & unit = processing_units[current_unit_number];

        unit.block_ext.block.clear();
        unit.block_ext.block_missing_values.clear();

        // We don't know how many blocks will be. So we have to read them all
        // until an empty block occured.
        Block block;
        while (!finished && (block = unit.parser->read()) != Block())
        {
            unit.block_ext.block.emplace_back(block);
            unit.block_ext.block_missing_values.emplace_back(unit.parser->getMissingValues());
        }

        if (!finished)
        {
            std::unique_lock lock(mutex);
            unit.status = READY_TO_READ;
            reader_condvar.notify_all();
        }
    }
    catch (...)
    {
        onBackgroundException();
    }
}

void ParallelParsingBlockInputStream::onBackgroundException()
{
    tryLogCurrentException(__PRETTY_FUNCTION__);

    std::unique_lock lock(mutex);
    if (!background_exception)
    {
        background_exception = std::current_exception();
    }
    finished = true;
    reader_condvar.notify_all();
    segmentator_condvar.notify_all();
}

Block ParallelParsingBlockInputStream::readImpl()
{
    if (isCancelledOrThrowIfKilled() || finished)
    {
        /**
          * Check for background exception and rethrow it before we return.
          */
        std::unique_lock lock(mutex);
        if (background_exception)
        {
            lock.unlock();
            cancel(false);
            std::rethrow_exception(background_exception);
        }

        return Block{};
    }

    const auto current_unit_number = reader_ticket_number % processing_units.size();
    auto & unit = processing_units[current_unit_number];

    if (!next_block_in_current_unit.has_value())
    {
        // We have read out all the Blocks from the previous Processing Unit,
        // wait for the current one to become ready.
        std::unique_lock lock(mutex);
        reader_condvar.wait(lock, [&](){ return unit.status == READY_TO_READ || finished; });

        if (finished)
        {
            /**
              * Check for background exception and rethrow it before we return.
              */
            if (background_exception)
            {
                lock.unlock();
                cancel(false);
                std::rethrow_exception(background_exception);
            }

            return Block{};
        }

        assert(unit.status == READY_TO_READ);
        next_block_in_current_unit = 0;
    }

    if (unit.block_ext.block.size() == 0)
    {
        assert(unit.is_last);
        finished = true;
        return Block{};
    }

    assert(next_block_in_current_unit.value() < unit.block_ext.block.size());

    Block res = std::move(unit.block_ext.block.at(*next_block_in_current_unit));
    last_block_missing_values = std::move(unit.block_ext.block_missing_values[*next_block_in_current_unit]);

    next_block_in_current_unit.value() += 1;

    if (next_block_in_current_unit.value() == unit.block_ext.block.size())
    {
        // Finished reading this Processing Unit, move to the next one.
        next_block_in_current_unit.reset();
        ++reader_ticket_number;

        if (unit.is_last)
        {
            // It it was the last unit, we're finished.
            finished = true;
        }
        else
        {
            // Pass the unit back to the segmentator.
            std::unique_lock lock(mutex);
            unit.status = READY_TO_INSERT;
            segmentator_condvar.notify_all();
        }
    }

    return res;
}


}