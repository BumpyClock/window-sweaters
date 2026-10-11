#pragma once

#include "KnitRaster.h"
#include <windows.h>
#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace window_sweaters::rendering {
class ResourceLedger final {
public:
    struct Statistics {
        std::size_t retainedCpuBytes{}, estimatedGpuBytes{}, uploadTransientBytes{};
        std::size_t peakCpuBytes{}, peakEstimatedGpuBytes{}, surfaces{}, reservations{};
        std::uint64_t admissions{}, quotaRejected{}, releases{}, releaseGeneration{};
        HRESULT error{};
    };
    class Lease final {
    public:
        Lease() = default;
        ~Lease() { Reset(); }
        Lease(Lease const&) = delete;
        Lease& operator=(Lease const&) = delete;
        Lease(Lease&& other) noexcept { Swap(other); }
        Lease& operator=(Lease&& other) noexcept { if (this!=&other) { Reset(); Swap(other); } return *this; }
        void Reset() noexcept {
            if (owner_) owner_->Release(*this);
            owner_=nullptr; perimeter_.reset(); gpu_=transient_=0;
        }
        void EndUpload() noexcept {
            if (!owner_ || !transient_) return;
            owner_->stats_.estimatedGpuBytes-=transient_;
            owner_->stats_.uploadTransientBytes-=transient_;
            gpu_-=transient_; transient_=0;
        }
    private:
        friend class ResourceLedger;
        void Swap(Lease& other) noexcept {
            std::swap(owner_,other.owner_); perimeter_.swap(other.perimeter_);
            std::swap(gpu_,other.gpu_); std::swap(transient_,other.transient_);
        }
        ResourceLedger* owner_{};
        std::shared_ptr<const KnitPerimeter> perimeter_;
        std::size_t gpu_{}, transient_{};
    };
    explicit ResourceLedger(std::size_t cpuLimit=128u*1024u*1024u,
        std::size_t gpuLimit=256u*1024u*1024u)
        : ownerThread_(GetCurrentThreadId()),cpuLimit_(cpuLimit),gpuLimit_(gpuLimit) {}
    Statistics Status() const noexcept { return stats_; }
    std::size_t CpuLimit() const noexcept { return cpuLimit_; }
    std::size_t EstimatedGpuLimit() const noexcept { return gpuLimit_; }
    HRESULT Reserve(std::shared_ptr<const KnitPerimeter> const& perimeter,Lease& lease) noexcept {
        if (GetCurrentThreadId()!=ownerThread_ || lease.owner_ || !perimeter || !perimeter->tile)
            return E_INVALIDARG;
        try {
            std::size_t cpu=sizeof(KnitPerimeter),gpu=0,transient=0;
            for (auto const& piece:perimeter->pieces) {
                auto const& image=piece.image;
                if (!image.width || !image.height || image.width>32768 || image.height>32768 ||
                    image.stride!=std::uint64_t(image.width)*4 ||
                    image.pixels.size()!=std::uint64_t(image.width)*image.height) return E_INVALIDARG;
                const auto surface=std::size_t(image.width)*image.height*4;
                if (!Add(gpu,surface) || !Add(cpu,image.pixels.capacity()*sizeof(std::uint32_t)))
                    return E_INVALIDARG;
                transient=std::max(transient,surface);
            }
            if (!Add(gpu,transient)) return E_INVALIDARG;
            auto const& tile=perimeter->tile;
            const auto chart=tile->spec.chart;
            std::size_t tileBytes=sizeof(knitting::Tile);
            if (!Add(tileBytes,tile->image.pixels.capacity()*sizeof(std::uint32_t)) ||
                !Add(tileBytes,tile->height.pixels.capacity()*sizeof(std::uint32_t))) return E_INVALIDARG;
            std::size_t chartBytes=chart?sizeof(knitting::Chart):0;
            if (chart && (!Add(chartBytes,chart->argbCells.capacity()*sizeof(std::uint32_t)) ||
                !Add(chartBytes,chart->semanticId.capacity()+1))) return E_INVALIDARG;
            std::size_t incoming=perimeters_.contains(perimeter.get())?0:cpu;
            if (!Add(incoming,tiles_.contains(tile.get())?0:tileBytes) ||
                !Add(incoming,chart && !charts_.contains(chart.get())?chartBytes:0)) return E_INVALIDARG;
            if (stats_.retainedCpuBytes>cpuLimit_ || incoming>cpuLimit_-stats_.retainedCpuBytes ||
                stats_.estimatedGpuBytes>gpuLimit_ || gpu>gpuLimit_-stats_.estimatedGpuBytes) {
                ++stats_.quotaRejected; stats_.error=HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
                return stats_.error;
            }
            try {
                perimeters_.try_emplace(perimeter.get(),Allocation{cpu,0});
                tiles_.try_emplace(tile.get(),Allocation{tileBytes,0});
                if (chart) charts_.try_emplace(chart.get(),Allocation{chartBytes,0});
            } catch (...) {
                EraseUnused(perimeters_); EraseUnused(tiles_); EraseUnused(charts_);
                throw;
            }
            ++perimeters_.at(perimeter.get()).references; ++tiles_.at(tile.get()).references;
            if (chart) ++charts_.at(chart.get()).references;
            lease.owner_=this; lease.perimeter_=perimeter; lease.gpu_=gpu; lease.transient_=transient;
            stats_.retainedCpuBytes+=incoming; stats_.estimatedGpuBytes+=gpu; stats_.uploadTransientBytes+=transient;
            stats_.peakCpuBytes=std::max(stats_.peakCpuBytes,stats_.retainedCpuBytes);
            stats_.peakEstimatedGpuBytes=std::max(stats_.peakEstimatedGpuBytes,stats_.estimatedGpuBytes);
            stats_.surfaces+=4; ++stats_.reservations; ++stats_.admissions; stats_.error=S_OK;
            return S_OK;
        } catch (...) { stats_.error=E_OUTOFMEMORY; return stats_.error; }
    }
private:
    struct Allocation { std::size_t bytes,references; };
    static bool Add(std::size_t& total,std::size_t value) noexcept {
        if (value>std::numeric_limits<std::size_t>::max()-total) return false;
        total+=value; return true;
    }
    template<class Map> static void EraseUnused(Map& values) { std::erase_if(values,[](auto const& pair) { return !pair.second.references; }); }
    template<class Map,class Key> void ReleaseAllocation(Map& values,Key key) noexcept {
        auto found=values.find(key);
        if (found==values.end()) return;
        if (!--found->second.references) { stats_.retainedCpuBytes-=found->second.bytes; values.erase(found); }
    }
    void Release(Lease const& lease) noexcept {
        auto const& perimeter=lease.perimeter_;
        ReleaseAllocation(perimeters_,perimeter.get());
        ReleaseAllocation(tiles_,perimeter->tile.get());
        if (perimeter->tile->spec.chart) ReleaseAllocation(charts_,perimeter->tile->spec.chart.get());
        stats_.estimatedGpuBytes-=lease.gpu_; stats_.uploadTransientBytes-=lease.transient_;
        stats_.surfaces-=4; --stats_.reservations; ++stats_.releases; ++stats_.releaseGeneration;
    }
    DWORD ownerThread_;
    std::size_t cpuLimit_,gpuLimit_;
    Statistics stats_;
    std::map<KnitPerimeter const*,Allocation> perimeters_;
    std::map<knitting::Tile const*,Allocation> tiles_;
    std::map<knitting::Chart const*,Allocation> charts_;
};
}
