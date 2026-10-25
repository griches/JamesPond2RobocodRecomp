// Quick save / load: the whole machine, taken between frames.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace amiga {

// One serialiser for both directions, so each part of the machine lists its
// state once.
class StateIO {
public:
    explicit StateIO(std::vector<uint8_t> *out) : out_(out) {}
    StateIO(const uint8_t *in, size_t size) : in_(in), size_(size) {}

    bool loading() const { return in_ != nullptr; }
    bool ok() const { return ok_; }

    void bytes(void *p, size_t n)
    {
        if (in_) {
            if (pos_ + n > size_) {
                ok_ = false;
                return;
            }
            std::memcpy(p, in_ + pos_, n);
            pos_ += n;
        } else {
            const uint8_t *b = static_cast<const uint8_t *>(p);
            out_->insert(out_->end(), b, b + n);
        }
    }
    template <class T> void io(T &v) { bytes(&v, sizeof v); }

private:
    std::vector<uint8_t> *out_ = nullptr;
    const uint8_t *in_ = nullptr;
    size_t size_ = 0, pos_ = 0;
    bool ok_ = true;
};

// Each part of the machine (defined beside its state).
void custom_state(StateIO &s);
void display_state(StateIO &s);
void paula_state(StateIO &s);
void cia_state(StateIO &s);
void cpu_state(StateIO &s);

// Format version of the save being loaded (older ones are still read).
extern uint32_t loading_version;

bool save_state(std::vector<uint8_t> &out);
bool load_state(const std::vector<uint8_t> &in, std::string *error);

} // namespace amiga
