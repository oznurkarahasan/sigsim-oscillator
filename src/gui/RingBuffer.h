#pragma once
#include <algorithm>
#include <vector>

// Fixed-capacity circular buffer for scope traces, matching the standard
// ImPlot "ScrollingBuffer" idiom: a plain float array plus an Offset into
// it, which ImPlot::PlotLine's own (count, offset) parameters know how to
// walk circularly without ever shifting elements around. Every trace on
// the scope (ADC input, biquad envelope, fade multiplier, bit decisions,
// ...) gets its own instance, all pushed once per SignalChain::step() in
// lockstep, so they always share the same size/offset.
class RingBuffer {
public:
    void setCapacity(int capacity) {
        capacity_ = std::max(1, capacity);
        data_.assign(static_cast<size_t>(capacity_), 0.0f);
        offset_ = 0;
        size_ = 0;
    }

    void push(float v) {
        data_[static_cast<size_t>(offset_)] = v;
        offset_ = (offset_ + 1) % capacity_;
        if (size_ < capacity_) ++size_;
    }

    void clear() {
        std::fill(data_.begin(), data_.end(), 0.0f);
        offset_ = 0;
        size_ = 0;
    }

    // Resize preserves nothing (per the development plan: "drop old
    // content -- no need to preserve history across a resize").
    void resize(int newCapacity) { setCapacity(newCapacity); }

    int capacity() const { return capacity_; }
    int size() const { return size_; }
    // ImPlot's "oldest element" index -- 0 while the buffer hasn't
    // wrapped yet, otherwise the current write position (which is about
    // to overwrite the oldest sample).
    int offset() const { return size_ < capacity_ ? 0 : offset_; }
    const float* data() const { return data_.data(); }

private:
    int capacity_ = 0;
    int offset_ = 0;
    int size_ = 0;
    std::vector<float> data_;
};
