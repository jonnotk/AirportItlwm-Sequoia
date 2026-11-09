#ifndef AIRPORTITLWM_PACKET_TRANSPORT_HPP
#define AIRPORTITLWM_PACKET_TRANSPORT_HPP

#include <stddef.h>
#include <stdint.h>

// All methods run under the same workloop gate as the native queue callbacks.
// Operations owns platform calls; this core owns every accepted packet until a
// counted completion callback transfers it or shutdown returns it to its pool.
template <typename Packet, typename Input, uint32_t Capacity>
class AirportItlwmPacketTransport {
    struct Ring {
        Packet *packets[Capacity];
        uint32_t head;
        uint32_t count;
        void reset() { head = count = 0; }
        bool push(Packet *packet) {
            if (count == Capacity) return false;
            packets[(head + count) % Capacity] = packet;
            ++count;
            return true;
        }
        Packet *pop() {
            if (!count) return nullptr;
            Packet *packet = packets[head];
            packets[head] = nullptr;
            head = (head + 1) % Capacity;
            --count;
            return packet;
        }
        bool contains(const Packet *packet) const {
            for (uint32_t i = 0; i < count; ++i)
                if (packets[(head + i) % Capacity] == packet) return true;
            return false;
        }
    };
    Ring tx;
    Ring rx;
    bool enabled;
    uint32_t maximumFrame;
    // Guards: one packet object is owned at most once, and the native
    // submit/complete callbacks never nest. Counters are for diagnostics.
    bool inSubmit;
    bool inTake;
    uint32_t duplicateSubmits;
    uint32_t duplicateCompletions;
    uint32_t nestedCalls;

    template <typename Operations>
    bool validPacket(Packet *packet, Operations &ops, uint32_t &length,
                     typename Operations::Descriptor &descriptor, uint64_t &offset) {
        if (!packet || ops.bufferCount(packet) != 1) return false;
        length = ops.packetLength(packet);
        offset = ops.packetOffset(packet);
        descriptor = ops.descriptor(packet);
        return descriptor && validRange(ops.descriptorLength(descriptor), offset, length, maximumFrame);
    }

public:
    static_assert(Capacity > 0, "transport capacity must be positive");

    // Called only before acquisition, or after drain has returned every packet.
    void initialize(uint32_t maximum) {
        tx.reset();
        rx.reset();
        enabled = false;
        maximumFrame = maximum;
        inSubmit = inTake = false;
        duplicateSubmits = duplicateCompletions = nestedCalls = 0;
    }

    static bool validRange(uint64_t descriptorLength, uint64_t offset,
                           uint64_t length, uint32_t maximum) {
        return length >= 14 && length <= maximum && offset <= descriptorLength &&
               length <= descriptorLength - offset;
    }

    // In-band IEEE 802.1Q customer/service tags remain in the copied bytes.
    // Count each tag in native link-header metadata. The metadata setter takes
    // one byte, and finalize requires payload beyond the complete L2 header.
    template <typename Reader>
    static bool ethernetHeaderLength(uint64_t frameLength, Reader read, uint8_t &result) {
        uint32_t header = 14;
        uint8_t type[2] = {};
        if (frameLength <= header || !read(12, type, 2)) return false;
        while ((type[0] == 0x81 && type[1] == 0x00) ||
               (type[0] == 0x88 && type[1] == 0xa8)) {
            if (header > 255 - 4 || frameLength <= header + 4) return false;
            header += 4;
            if (!read(header - 2, type, 2)) return false;
        }
        result = static_cast<uint8_t>(header);
        return true;
    }

    bool isEnabled() const { return enabled; }
    uint32_t txPending() const { return tx.count; }
    uint32_t rxPending() const { return rx.count; }
    uint32_t txFree() const { return enabled ? Capacity - tx.count : 0; }
    uint32_t duplicateSubmitCount() const { return duplicateSubmits; }
    uint32_t duplicateCompletionCount() const { return duplicateCompletions; }
    uint32_t nestedCallCount() const { return nestedCalls; }

    template <typename Operations>
    void enable(Operations &ops) {
        enabled = true;
        // Native checkForWork can consume a notification while its queue is
        // disabled. Always re-arm pending work after the queues are enabled.
        if (tx.count) ops.requestTxCompletion();
        if (rx.count) ops.requestRxCompletion();
        ops.requestTxSubmission();
    }

    template <typename Operations>
    uint32_t submit(Packet *const *packets, uint32_t count, Operations &ops) {
        if (!enabled || !packets) return 0;
        if (inSubmit || inTake) {
            // A nested dequeue would see the same unadvanced read index and
            // hand over packets this core already owns.
            ++nestedCalls;
            ops.requestTxRetry();
            return 0;
        }
        inSubmit = true;
        uint32_t consumed = 0;
        while (consumed < count && tx.count < Capacity) {
            if (!ops.outputReady()) {
                ops.requestTxRetry();
                break;
            }
            Packet *packet = packets[consumed];
            if (!packet) break;
            if (tx.contains(packet)) {
                // Already owned and pending completion: never send or own it
                // twice. Count it consumed so the native read index advances.
                ++duplicateSubmits;
                ++consumed;
                continue;
            }
            uint32_t length = 0;
            uint64_t offset = 0;
            typename Operations::Descriptor descriptor = {};
            if (!validPacket(packet, ops, length, descriptor, offset)) {
                ops.markTxDropped(packet);
            } else {
                Input input = ops.allocateInput(length);
                if (!input) {
                    ops.requestTxRetry();
                    break; // No ownership transfer, retry this prefix.
                }
                if (!ops.copyTx(descriptor, offset, input, length)) {
                    ops.freeInput(input);
                    ops.requestTxRetry();
                    break;
                }
                // submitInput consumes the mbuf on every return, including a
                // HAL drop. Completion concerns copied buffer ownership.
                ops.submitInput(input);
            }
            tx.push(packet); // Capacity was reserved before copy or HAL calls.
            ++consumed;
        }
        inSubmit = false;
        if (consumed) ops.requestTxCompletion(); // Native async option 0x1.
        return consumed;
    }

    template <typename Operations>
    void receive(Input input, Operations &ops) {
        if (!input) return;
        const uint64_t length = ops.inputLength(input);
        // Native kern_packet_finalize requires headroom + link header length
        // to be strictly smaller than the first buffer's data length.
        if (!enabled || rx.count == Capacity || length <= 14 || length > maximumFrame) {
            ops.freeInput(input);
            return;
        }
        Packet *packet = ops.allocateRx();
        if (!packet) {
            ops.freeInput(input);
            return;
        }
        bool ready = ops.prepareRx(packet) && ops.bufferCount(packet) == 1 &&
                     ops.configureRx(packet, input, static_cast<uint32_t>(length));
        typename Operations::Descriptor descriptor = {};
        if (ready) descriptor = ops.descriptor(packet);
        const uint64_t offset = ready ? ops.packetOffset(packet) : 0;
        ready = descriptor && validRange(ops.descriptorLength(descriptor), offset, length, maximumFrame) &&
                ops.copyRx(input, descriptor, offset, static_cast<uint32_t>(length));
        ops.freeInput(input);
        if (!ready) {
            ops.returnPacket(packet);
            return;
        }
        rx.push(packet);
        // Ownership remains here even if notification fails or is suppressed.
        ops.requestRxCompletion();
    }

    template <typename Operations>
    uint32_t takeTx(Packet **output, uint32_t capacity, Operations &ops) {
        if (!enabled || !output) return 0;
        if (inSubmit || inTake) {
            ++nestedCalls;
            if (tx.count) ops.requestTxCompletion();
            return 0;
        }
        inTake = true;
        uint32_t count = 0;
        while (count < capacity && tx.count) {
            Packet *packet = tx.pop();
            bool seen = !packet;
            for (uint32_t i = 0; !seen && i < count; ++i) seen = output[i] == packet;
            if (seen) {
                // The native batch free disposes each object in order; a
                // repeated or null object faults the kernel packet free.
                ++duplicateCompletions;
                continue;
            }
            output[count++] = packet;
        }
        inTake = false;
        if (tx.count) ops.requestTxCompletion();
        if (count) ops.requestTxSubmission();
        return count;
    }

    template <typename Operations>
    uint32_t takeRx(Packet **output, uint32_t capacity, Operations &ops) {
        if (!enabled || !output) return 0;
        uint32_t count = 0;
        while (count < capacity && rx.count) output[count++] = rx.pop();
        if (rx.count) ops.requestRxCompletion();
        return count;
    }

    template <typename Operations>
    void disableAndDrain(Operations &ops) {
        enabled = false;
        while (tx.count) ops.returnPacket(tx.pop());
        while (rx.count) ops.returnPacket(rx.pop());
    }
};

#endif
