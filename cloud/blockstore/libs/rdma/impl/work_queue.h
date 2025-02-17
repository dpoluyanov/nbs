#pragma once

#include "public.h"

#include <rdma/rdma_verbs.h>

#include <util/generic/utility.h>

namespace NCloud::NBlockStore::NRdma {

////////////////////////////////////////////////////////////////////////////////

constexpr size_t RDMA_MAX_SEND_SGE = 1;
constexpr size_t RDMA_MAX_RECV_SGE = 1;

////////////////////////////////////////////////////////////////////////////////

struct TSendWr
{
    ibv_send_wr wr;
    ibv_sge sg_list[RDMA_MAX_SEND_SGE];

    void* context;

    TSendWr()
    {
        Zero(*this);
    }

    template <typename T>
    T* Message()
    {
        return reinterpret_cast<T*>(wr.sg_list[0].addr);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TRecvWr
{
    ibv_recv_wr wr;
    ibv_sge sg_list[RDMA_MAX_RECV_SGE];

    void* context;

    TRecvWr()
    {
        Zero(*this);
    }

    template <typename T>
    T* Message()
    {
        return reinterpret_cast<T*>(wr.sg_list[0].addr);
    }
};

////////////////////////////////////////////////////////////////////////////////

template <typename T>
class TWorkQueue
{
private:
    T* Head = nullptr;
    size_t Size_ = 0;

public:
    void Push(T* wr)
    {
        Y_VERIFY_DEBUG(wr);
        wr->wr.next = Head ? &Head->wr : nullptr;
        wr->context = nullptr;
        Head = wr;
        Size_++;
    }

    T* Pop()
    {
        auto* wr = Head;
        if (wr) {
            Head = reinterpret_cast<T*>(wr->wr.next);
            wr->wr.next = nullptr;
            Size_--;
        }
        return wr;
    }

    void Clear()
    {
        Head = nullptr;
        Size_ = 0;
    }

    size_t Size() const
    {
        return Size_;
    }
};

}   // namespace NCloud::NBlockStore::NRdma
