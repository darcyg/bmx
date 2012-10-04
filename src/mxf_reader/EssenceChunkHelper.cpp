/*
 * Copyright (C) 2010, British Broadcasting Corporation
 * All Rights Reserved.
 *
 * Author: Philip de Nier
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the British Broadcasting Corporation nor the names
 *       of its contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define __STDC_FORMAT_MACROS

#include <cstdio>

#include <memory>

#include <libMXF++/MXF.h>

#include <bmx/mxf_reader/EssenceChunkHelper.h>
#include <bmx/mxf_reader/MXFFileReader.h>
#include <bmx/mxf_helper/PictureMXFDescriptorHelper.h>
#include <bmx/BMXException.h>
#include <bmx/Logging.h>

#include <mxf/mxf_avid.h>

using namespace std;
using namespace bmx;
using namespace mxfpp;



EssenceChunk::EssenceChunk()
{
    file_position = 0;
    essence_offset = 0;
    size = 0;
    is_complete = false;
    partition_id = 0;
}



EssenceChunkHelper::EssenceChunkHelper(MXFFileReader *file_reader)
{
    mFileReader = file_reader;
    mAvidFirstFrameOffset = 0;
    mLastEssenceChunk = 0;
    mNumIndexedPartitions = 0;
    mIsComplete = false;

    if (mFileReader->IsClipWrapped() &&
        mFileReader->GetInternalTrackReader(0)->GetTrackInfo()->is_picture)
    {
        auto_ptr<MXFDescriptorHelper> helper(MXFDescriptorHelper::Create(
            mFileReader->GetInternalTrackReader(0)->GetFileDescriptor(),
            mFileReader->GetMXFVersion(),
            mFileReader->GetInternalTrackReader(0)->GetTrackInfo()->essence_container_label));
        PictureMXFDescriptorHelper *picture_helper = dynamic_cast<PictureMXFDescriptorHelper*>(helper.get());

        if (picture_helper->HaveAvidFirstFrameOffset())
            mAvidFirstFrameOffset = picture_helper->GetAvidFirstFrameOffset();
    }
}

EssenceChunkHelper::~EssenceChunkHelper()
{
}

void EssenceChunkHelper::CreateEssenceChunkIndex()
{
    mxfKey key;
    uint8_t llen;
    uint64_t len;
    File *mxf_file = mFileReader->mFile;

    const vector<Partition*> &partitions = mFileReader->mFile->getPartitions();
    size_t i;
    for (i = 0; i < partitions.size(); i++) {
        if (partitions[i]->getBodySID() != mFileReader->mBodySID)
            continue;

        int64_t partition_end;
        if (i + 1 < partitions.size())
            partition_end = partitions[i + 1]->getThisPartition();
        else
            partition_end = mxf_file->size();

        mxf_file->seek(partitions[i]->getThisPartition(), SEEK_SET);
        mxf_file->readKL(&key, &llen, &len);
        mxf_file->skip(len);
        while (!mxf_file->eof())
        {
            mxf_file->readNextNonFillerKL(&key, &llen, &len);

            if (mxf_is_partition_pack(&key)) {
                break;
            } else if (mxf_is_header_metadata(&key)) {
                if (partitions[i]->getHeaderByteCount() > mxfKey_extlen + llen + len)
                    mxf_file->skip(partitions[i]->getHeaderByteCount() - (mxfKey_extlen + llen));
                else
                    mxf_file->skip(len);
            } else if (mxf_is_index_table_segment(&key)) {
                if (partitions[i]->getIndexByteCount() > mxfKey_extlen + llen + len)
                    mxf_file->skip(partitions[i]->getIndexByteCount() - (mxfKey_extlen + llen));
                else
                    mxf_file->skip(len);
            } else if (mxf_is_gc_essence_element(&key) || mxf_avid_is_essence_element(&key)) {
                if (mFileReader->IsClipWrapped()) {
                    // check whether this is the target essence container; skip and continue if not
                    if (!mFileReader->GetInternalTrackReaderByNumber(mxf_get_track_number(&key))) {
                        mxf_file->skip(len);
                        continue;
                    }
                }

                AppendChunk(i, mxf_file->tell(), llen, len);
                if (mFileReader->IsFrameWrapped()) {
                    UpdateLastChunk(partition_end, true);
                    break;
                } else {
                    // continue with clip-wrapped to support multiple essence container elements in this partition
                    mxf_file->skip(len);
                }
            } else {
                mxf_file->skip(len);
            }
        }
    }

    mIsComplete = true;
}

void EssenceChunkHelper::AppendChunk(size_t partition_id, int64_t file_position, uint8_t klv_llen, uint64_t klv_len)
{
    // Note: file_position is after the KL

    Partition *partition = mFileReader->mFile->getPartitions()[partition_id];

    // check the essence container data is contiguous
    uint64_t body_offset = partition->getBodyOffset();
    if (mEssenceChunks.empty()) {
        if (body_offset > 0) {
            log_warn("Ignoring potential missing essence container data; "
                     "partition pack's BodyOffset 0x%"PRIx64" > expected offset 0x00\n",
                     body_offset);
            body_offset = 0;
        }
    } else if (body_offset > (uint64_t)(mEssenceChunks.back().essence_offset + mEssenceChunks.back().size)) {
        log_warn("Ignoring potential missing essence container data; "
                 "partition pack's BodyOffset 0x%"PRIx64" > expected offset 0x%"PRIx64"\n",
                 body_offset, mEssenceChunks.back().essence_offset + mEssenceChunks.back().size);
        body_offset = mEssenceChunks.back().essence_offset + mEssenceChunks.back().size;
    } else if (body_offset < (uint64_t)(mEssenceChunks.back().essence_offset + mEssenceChunks.back().size)) {
        log_warn("Ignoring potential overlapping essence container data; "
                 "partition pack's BodyOffset 0x%"PRIx64" < expected offset 0x%"PRIx64"\n",
                 body_offset, mEssenceChunks.back().essence_offset + mEssenceChunks.back().size);
        body_offset = mEssenceChunks.back().essence_offset + mEssenceChunks.back().size;
    }

    // add this partition's essence to the index
    EssenceChunk essence_chunk;
    essence_chunk.essence_offset = body_offset;
    essence_chunk.file_position = file_position;
    essence_chunk.partition_id = partition_id;
    if (mFileReader->IsFrameWrapped()) {
        essence_chunk.file_position -= mxfKey_extlen + klv_llen;
        essence_chunk.size = 0;
        essence_chunk.is_complete = false;
    } else {
        essence_chunk.size = klv_len;
        if (mAvidFirstFrameOffset > 0 && mEssenceChunks.empty()) {
            essence_chunk.file_position += mAvidFirstFrameOffset;
            essence_chunk.size -= mAvidFirstFrameOffset;
        }
        BMX_CHECK(essence_chunk.size >= 0);
        essence_chunk.is_complete = true;
    }
    mEssenceChunks.push_back(essence_chunk);

    mNumIndexedPartitions = partition_id + 1;
}

void EssenceChunkHelper::UpdateLastChunk(int64_t file_position, bool is_end)
{
    if (!mEssenceChunks.empty() &&
        !mEssenceChunks.back().is_complete &&
        file_position >= mEssenceChunks.back().file_position + mEssenceChunks.back().size)
    {
        mEssenceChunks.back().size = file_position - mEssenceChunks.back().file_position;
        mEssenceChunks.back().is_complete = is_end;
    }
}

void EssenceChunkHelper::SetIsComplete()
{
    mIsComplete = true;
}

bool EssenceChunkHelper::HaveFilePosition(int64_t essence_offset)
{
    if (mEssenceChunks.empty())
        return false;

    EssenceOffsetUpdate(essence_offset);

    return mEssenceChunks[mLastEssenceChunk].essence_offset <= essence_offset &&
           mEssenceChunks[mLastEssenceChunk].essence_offset + mEssenceChunks[mLastEssenceChunk].size >= essence_offset;
}

int64_t EssenceChunkHelper::GetEssenceDataSize() const
{
    if (mEssenceChunks.empty())
        return 0;
    else
        return mEssenceChunks.back().essence_offset + mEssenceChunks.back().size;
}

int64_t EssenceChunkHelper::GetFilePosition(int64_t essence_offset, int64_t size)
{
    EssenceOffsetUpdate(essence_offset);

    bool have_position = true;
    if (mEssenceChunks[mLastEssenceChunk].essence_offset > essence_offset)
    {
        have_position = false;
    }
    else if (mEssenceChunks[mLastEssenceChunk].essence_offset + mEssenceChunks[mLastEssenceChunk].size <
                essence_offset + size)
    {
        if (mEssenceChunks[mLastEssenceChunk].essence_offset + mEssenceChunks[mLastEssenceChunk].size < essence_offset)
            have_position = false;
        else if (mEssenceChunks[mLastEssenceChunk].is_complete)
            have_position = false;
    }
    if (!have_position) {
        BMX_EXCEPTION(("Failed to find edit unit (off=0x%"PRIx64",size=0x%"PRIx64") in essence container",
                       essence_offset, size));
    }

    return mEssenceChunks[mLastEssenceChunk].file_position +
                (essence_offset - mEssenceChunks[mLastEssenceChunk].essence_offset);
}

int64_t EssenceChunkHelper::GetFilePosition(int64_t essence_offset)
{
    EssenceOffsetUpdate(essence_offset);

    if (mEssenceChunks[mLastEssenceChunk].essence_offset > essence_offset ||
        mEssenceChunks[mLastEssenceChunk].essence_offset + mEssenceChunks[mLastEssenceChunk].size < essence_offset)
    {
        BMX_EXCEPTION(("Failed to find edit unit offset (off=0x%"PRIx64") in essence container", essence_offset));
    }

    return mEssenceChunks[mLastEssenceChunk].file_position +
                (essence_offset - mEssenceChunks[mLastEssenceChunk].essence_offset);
}

int64_t EssenceChunkHelper::GetEssenceOffset(int64_t file_position)
{
    FilePositionUpdate(file_position);

    if (mEssenceChunks[mLastEssenceChunk].file_position > file_position ||
        mEssenceChunks[mLastEssenceChunk].file_position + mEssenceChunks[mLastEssenceChunk].size < file_position)
    {
        BMX_EXCEPTION(("Failed to find edit unit file position (pos=0x%"PRIx64") in essence container", file_position));
    }

    return mEssenceChunks[mLastEssenceChunk].essence_offset +
                (file_position - mEssenceChunks[mLastEssenceChunk].file_position);
}

void EssenceChunkHelper::EssenceOffsetUpdate(int64_t essence_offset)
{
    BMX_CHECK(!mEssenceChunks.empty());

    // TODO: use binary search
    if (mEssenceChunks[mLastEssenceChunk].essence_offset > essence_offset)
    {
        // edit unit is in chunk before mLastEssenceChunk
        size_t i;
        for (i = mLastEssenceChunk; i > 0; i--) {
            if (mEssenceChunks[i - 1].essence_offset <= essence_offset) {
                mLastEssenceChunk = i - 1;
                break;
            }
        }
    }
    else if (mEssenceChunks[mLastEssenceChunk].essence_offset +
                    mEssenceChunks[mLastEssenceChunk].size <= essence_offset)
    {
        // edit unit is in chunk after mLastEssenceChunk
        size_t i;
        for (i = mLastEssenceChunk + 1; i < mEssenceChunks.size(); i++) {
            if (mEssenceChunks[i].essence_offset + mEssenceChunks[i].size > essence_offset) {
                mLastEssenceChunk = i;
                break;
            }
        }
    }
}

void EssenceChunkHelper::FilePositionUpdate(int64_t file_position)
{
    BMX_CHECK(!mEssenceChunks.empty());

    // TODO: use binary search
    if (mEssenceChunks[mLastEssenceChunk].file_position > file_position)
    {
        // edit unit is in chunk before mLastEssenceChunk
        size_t i;
        for (i = mLastEssenceChunk; i > 0; i--) {
            if (mEssenceChunks[i - 1].file_position <= file_position) {
                mLastEssenceChunk = i - 1;
                break;
            }
        }
    }
    else if (mEssenceChunks[mLastEssenceChunk].file_position +
                 mEssenceChunks[mLastEssenceChunk].size <= file_position)
    {
        // edit unit is in chunk after mLastEssenceChunk
        size_t i;
        for (i = mLastEssenceChunk + 1; i < mEssenceChunks.size(); i++) {
            if (mEssenceChunks[i].file_position + mEssenceChunks[i].size > file_position) {
                mLastEssenceChunk = i;
                break;
            }
        }
    }
}

