#pragma once

#include "KnitRaster.h"
#include <windows.h>
#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace window_sweaters::rendering {
class ResourceLedger final {
public:
    struct Statistics {
        std::size_t retainedCpuBytes{}, estimatedGpuBytes{}, uploadTransientBytes{};
        std::size_t peakCpuBytes{}, peakEstimatedGpuBytes{}, surfaces{}, reservations{};
        std::size_t cachedGpuBytes{}, cacheEntries{};
        std::uint64_t admissions{}, quotaRejected{}, releases{}, releaseGeneration{};
        std::uint64_t cacheAdmissions{}, cacheReleases{};
        HRESULT error{};
    };
    class CacheLease final {
    public:
        CacheLease() = default;
        ~CacheLease() { Reset(); }
        CacheLease(CacheLease const&) = delete;
        CacheLease& operator=(CacheLease const&) = delete;
        CacheLease(CacheLease&& other) noexcept { Swap(other); }
        CacheLease& operator=(CacheLease&& other) noexcept {
            if (this!=&other) { Reset(); Swap(other); }
            return *this;
        }
        void Reset() noexcept {
            if (owner_) {
                owner_->stats_.estimatedGpuBytes-=bytes_;
                owner_->stats_.cachedGpuBytes-=bytes_;
                --owner_->stats_.cacheEntries;
                ++owner_->stats_.cacheReleases;
                ++owner_->stats_.releaseGeneration;
            }
            owner_=nullptr; bytes_=0;
        }
    private:
        friend class ResourceLedger;
        void Swap(CacheLease& other) noexcept {
            std::swap(owner_,other.owner_); std::swap(bytes_,other.bytes_);
        }
        ResourceLedger* owner_{};
        std::size_t bytes_{};
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
            cachePins_.clear();
            owner_=nullptr; perimeter_.reset(); gpu_=transient_=cacheBudget_=0;
        }
        void EndUpload() noexcept {
            cachePins_.clear();
            if (!owner_ || !transient_) return;
            owner_->stats_.estimatedGpuBytes-=transient_;
            owner_->stats_.uploadTransientBytes-=transient_;
            gpu_-=transient_; transient_=cacheBudget_=0;
        }
    private:
        friend class ResourceLedger;
        friend class GpuShader;
        friend class KnitAssembler;
        enum class CacheKind { Coordinates, Tile };
        struct CachePin {
            void const* cache;
            CacheKind kind;
            std::shared_ptr<void> allocation;
        };
        template<class Entry, class Predicate>
        std::shared_ptr<Entry> FindCache(void const* cache,CacheKind kind,Predicate predicate) const noexcept {
            for (auto const& pin:cachePins_) {
                if (pin.cache!=cache || pin.kind!=kind) continue;
                auto entry=std::static_pointer_cast<Entry>(pin.allocation);
                if (predicate(*entry)) return entry;
            }
            return {};
        }
        template<class Entry>
        HRESULT PinCache(void const* cache,CacheKind kind,std::shared_ptr<Entry> const& entry) noexcept {
            for (auto const& pin:cachePins_)
                if (pin.cache==cache && pin.kind==kind && pin.allocation.get()==entry.get()) return S_OK;
            try { cachePins_.push_back({cache,kind,entry}); return S_OK; }
            catch (...) { return E_OUTOFMEMORY; }
        }
        void Swap(Lease& other) noexcept {
            std::swap(owner_,other.owner_); perimeter_.swap(other.perimeter_);
            std::swap(gpu_,other.gpu_); std::swap(transient_,other.transient_);
            std::swap(cacheBudget_,other.cacheBudget_);
            cachePins_.swap(other.cachePins_);
        }
        ResourceLedger* owner_{};
        std::shared_ptr<const KnitPerimeter> perimeter_;
        std::size_t gpu_{}, transient_{}, cacheBudget_{};
        std::vector<CachePin> cachePins_;
    };
    explicit ResourceLedger(std::size_t cpuLimit=128u*1024u*1024u,
        std::size_t gpuLimit=256u*1024u*1024u)
        : ownerThread_(GetCurrentThreadId()),cpuLimit_(cpuLimit),gpuLimit_(gpuLimit) {}
    Statistics Status() const noexcept { return stats_; }
    std::size_t CpuLimit() const noexcept { return cpuLimit_; }
    std::size_t EstimatedGpuLimit() const noexcept { return gpuLimit_; }
    std::size_t AvailableGpuBytes() const noexcept {
        return stats_.estimatedGpuBytes<gpuLimit_?gpuLimit_-stats_.estimatedGpuBytes:0;
    }
    std::size_t CacheCapacity(Lease const* upload) const noexcept {
        return AvailableGpuBytes()+(upload && upload->owner_==this?upload->cacheBudget_:0);
    }
    HRESULT ReserveCache(std::size_t bytes,CacheLease& lease,Lease* upload=nullptr) noexcept {
        if (GetCurrentThreadId()!=ownerThread_ || lease.owner_ || !bytes ||
            (upload && upload->owner_!=this)) return E_INVALIDARG;
        const auto prepaid=upload?std::min(bytes,upload->cacheBudget_):0;
        if (bytes-prepaid>AvailableGpuBytes()) {
            ++stats_.quotaRejected; stats_.error=HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
            return stats_.error;
        }
        lease.owner_=this; lease.bytes_=bytes;
        if (prepaid) {
            upload->gpu_-=prepaid; upload->transient_-=prepaid; upload->cacheBudget_-=prepaid;
            stats_.uploadTransientBytes-=prepaid;
        }
        stats_.estimatedGpuBytes+=bytes-prepaid; stats_.cachedGpuBytes+=bytes;
        ++stats_.cacheEntries; ++stats_.cacheAdmissions;
        stats_.peakEstimatedGpuBytes=std::max(stats_.peakEstimatedGpuBytes,stats_.estimatedGpuBytes);
        stats_.error=S_OK;
        return S_OK;
    }
    HRESULT Reserve(std::shared_ptr<const KnitPerimeter> const& perimeter,Lease& lease) noexcept {
        if (GetCurrentThreadId()!=ownerThread_ || lease.owner_ || !perimeter || !perimeter->tile)
            return E_INVALIDARG;
        try {
            std::size_t cpu=sizeof(KnitPerimeter),gpu=0,transient=0,cacheBudget=0;
            for (auto const& piece:perimeter->pieces) {
                if (!perimeter->PieceValid(piece) || piece.width>32768 || piece.height>32768) return E_INVALIDARG;
                const auto surface=std::size_t(piece.width)*piece.height*4;
                if (!Add(gpu,surface) || !Add(cpu,piece.image.pixels.capacity()*sizeof(std::uint32_t)))
                    return E_INVALIDARG;
                if (perimeter->assembly) {
                    if (!Add(transient,surface*2u) || !Add(cacheBudget,(std::size_t(piece.width)+piece.height)*8u))
                        return E_INVALIDARG;
                } else transient=std::max(transient,surface);
            }
            const auto tiles=perimeter->RetainedTiles();
            const auto chart=perimeter->tile->spec.chart;
            std::array<std::size_t,3> tileBytes{};
            for (std::size_t i=0;i<tiles.size();++i) {
                if (!tiles[i]) continue;
                if (perimeter->assembly && (!Add(cacheBudget,std::size_t(tiles[i]->image.width)*tiles[i]->image.height*4u) ||
                    !Add(cacheBudget,std::size_t(tiles[i]->height.width)*tiles[i]->height.height*4u))) return E_INVALIDARG;
                tileBytes[i]=sizeof(knitting::Tile);
                if (!Add(tileBytes[i],tiles[i]->image.pixels.capacity()*sizeof(std::uint32_t)) ||
                    !Add(tileBytes[i],tiles[i]->height.pixels.capacity()*sizeof(std::uint32_t))) return E_INVALIDARG;
            }
            std::size_t chartBytes=chart?sizeof(knitting::Chart):0;
            if (chart && (!Add(chartBytes,chart->argbCells.capacity()*sizeof(std::uint32_t)) ||
                !Add(chartBytes,chart->semanticId.capacity()+1))) return E_INVALIDARG;
            if (!Add(transient,cacheBudget) || !Add(gpu,transient)) return E_INVALIDARG;
            std::size_t incoming=perimeters_.contains(perimeter.get())?0:cpu;
            for (std::size_t i=0;i<tiles.size();++i)
                if (tiles[i] && !tiles_.contains(tiles[i]) && !Add(incoming,tileBytes[i])) return E_INVALIDARG;
            if (!Add(incoming,chart && !charts_.contains(chart.get())?chartBytes:0)) return E_INVALIDARG;
            if (stats_.retainedCpuBytes>cpuLimit_ || incoming>cpuLimit_-stats_.retainedCpuBytes ||
                stats_.estimatedGpuBytes>gpuLimit_ || gpu>gpuLimit_-stats_.estimatedGpuBytes) {
                ++stats_.quotaRejected; stats_.error=HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
                return stats_.error;
            }
            try {
                perimeters_.try_emplace(perimeter.get(),Allocation{cpu,0});
                for (std::size_t i=0;i<tiles.size();++i)
                    if (tiles[i]) tiles_.try_emplace(tiles[i],Allocation{tileBytes[i],0});
                if (chart) charts_.try_emplace(chart.get(),Allocation{chartBytes,0});
            } catch (...) {
                EraseUnused(perimeters_); EraseUnused(tiles_); EraseUnused(charts_);
                throw;
            }
            ++perimeters_.at(perimeter.get()).references;
            for (const auto* tile:tiles)
                if (tile) ++tiles_.at(tile).references;
            if (chart) ++charts_.at(chart.get()).references;
            lease.owner_=this; lease.perimeter_=perimeter; lease.gpu_=gpu; lease.transient_=transient;
            lease.cacheBudget_=cacheBudget;
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
        for (const auto* tile:perimeter->RetainedTiles())
            if (tile) ReleaseAllocation(tiles_,tile);
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
