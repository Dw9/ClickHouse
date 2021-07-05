#include "Lz4DeflatingWriteBuffer.h"
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int LZ4_ENCODER_FAILED;
}

Lz4DeflatingWriteBuffer::Lz4DeflatingWriteBuffer(
    std::unique_ptr<WriteBuffer> out_, int /*compression_level*/, size_t buf_size, char * existing_memory, size_t alignment)
    : BufferWithOwnMemory<WriteBuffer>(buf_size, existing_memory, alignment), out(std::move(out_))
{
    count_in = 0;
    compression_ctx = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
    if (LZ4F_isError(compression_ctx))
    {
        throw Exception(
            ErrorCodes::LZ4_ENCODER_FAILED,
            "creation of LZ$ compression context failed. LZ4F version: {}",
            LZ4F_VERSION,
            ErrorCodes::LZ4_ENCODER_FAILED);
    }
}

Lz4DeflatingWriteBuffer::~Lz4DeflatingWriteBuffer()
{
    MemoryTracker::LockExceptionInThread lock(VariableContext::Global);
    finish();
    LZ4F_freeCompressionContext(ctx);
}

void Lz4DeflatingWriteBuffer::nextImpl()
{
    if (!offset())
        return;

    in_buff = reinterpret_cast<void *>(working_buffer.begin());
    in_chunk_size = offset();


    try
    {
        do
        {
            out->nextIfAtEnd();

            out_buff = reinterpret_cast<void *>(out->buffer().begin());
            out_capacity = out->buffer().end() - out->position();

            /// write frame header and check for errors
            {
                size_t const header_size = LZ4F_compressBegin(ctx, out_buff, out_capacity, nullptr);

                if (LZ4F_isError(header_size))
                {
                    throw Exception(
                        ErrorCodes::LZ4_ENCODER_FAILED,
                        "LZ4 failed to start stream encoding. LZ4F version: {}",
                        LZ4F_VERSION,
                        ErrorCodes::LZ4_ENCODER_FAILED);
                }
                count_out = header_size;
            }


            /// compress begin
            {
                const size_t compressed_size = LZ4F_compressUpdate(ctx, out_buff, out_capacity, in_buff, in_chunk_size, nullptr);

                if (LZ4F_isError(compressed_size))
                {
                    throw Exception(
                        ErrorCodes::LZ4_ENCODER_FAILED,
                        "LZ4 failed to encode stream. LZ4F version: {}",
                        LZ4F_VERSION,
                        ErrorCodes::LZ4_ENCODER_FAILED);
                }
                count_out += compressed_size;
            }
            out->position() = out->buffer().end() - count_out;
        } while (count_out < count_in);
    }
    catch (...)
    {
        out->position() = out->buffer().begin();
    }
}

void Lz4DeflatingWriteBuffer::finish()
{
    if (finished)
        return;

    try
    {
        finishImpl();
        out->finalize();
        finished = true;
    }
    catch (...)
    {
        /// Do not try to flush next time after exception.
        out->position() = out->buffer().begin();
        finished = true;
        throw;
    }
}

void Lz4DeflatingWriteBuffer::finishImpl()
{
    next();
    out->nextIfAtEnd();

    /// compression end
    const size_t end_size = LZ4F_compressEnd(ctx, out_buff, out_capacity, nullptr);

    if (LZ4F_isError(end_size))
    {
        throw Exception(
            ErrorCodes::LZ4_ENCODER_FAILED,
            "LZ4 failed to end stream encoding. LZ4F version: {}",
            LZ4F_VERSION,
            ErrorCodes::LZ4_ENCODER_FAILED);
    }
    count_out += end_size;
    out->position() = out->buffer().end() - count_out;
}

}
