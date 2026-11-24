#include "lumen/stemmer.hpp"

#include <cstring>

namespace lumen {
namespace {

// Direct port of Martin Porter's reference algorithm. b[0..k] is the live word;
// j is a general offset used by the step helpers.
class Porter {
public:
    explicit Porter(std::string w) : b_(std::move(w)), k_(static_cast<int>(b_.size()) - 1) {}

    std::string run() {
        if (k_ <= 1) return b_;
        step1ab();
        if (k_ > 0) {
            step1c();
            step2();
            step3();
            step4();
            step5();
        }
        return b_.substr(0, static_cast<size_t>(k_ + 1));
    }

private:
    bool cons(int i) const {
        switch (b_[i]) {
            case 'a': case 'e': case 'i': case 'o': case 'u': return false;
            case 'y': return i == 0 ? true : !cons(i - 1);
            default: return true;
        }
    }

    // Number of VC sequences in b[0..j].
    int m() const {
        int n = 0, i = 0;
        for (;;) {
            if (i > j_) return n;
            if (!cons(i)) break;
            ++i;
        }
        ++i;
        for (;;) {
            for (;;) {
                if (i > j_) return n;
                if (cons(i)) break;
                ++i;
            }
            ++i;
            ++n;
            for (;;) {
                if (i > j_) return n;
                if (!cons(i)) break;
                ++i;
            }
            ++i;
        }
    }

    bool vowelInStem() const {
        for (int i = 0; i <= j_; ++i)
            if (!cons(i)) return true;
        return false;
    }

    bool doubleC(int i) const {
        if (i < 1 || b_[i] != b_[i - 1]) return false;
        return cons(i);
    }

    bool cvc(int i) const {
        if (i < 2 || !cons(i) || cons(i - 1) || !cons(i - 2)) return false;
        const char ch = b_[i];
        return !(ch == 'w' || ch == 'x' || ch == 'y');
    }

    bool ends(const char* s) {
        const int len = static_cast<int>(std::strlen(s));
        if (len > k_ + 1) return false;
        if (b_.compare(static_cast<size_t>(k_ - len + 1), static_cast<size_t>(len), s) != 0) return false;
        j_ = k_ - len;
        return true;
    }

    void setTo(const char* s) {
        const int len = static_cast<int>(std::strlen(s));
        b_.replace(static_cast<size_t>(j_ + 1), std::string::npos, s);
        k_ = j_ + len;
    }

    void r(const char* s) {
        if (m() > 0) setTo(s);
    }

    void step1ab() {
        if (b_[k_] == 's') {
            if (ends("sses")) k_ -= 2;
            else if (ends("ies")) setTo("i");
            else if (b_[k_ - 1] != 's') --k_;
        }
        if (ends("eed")) {
            if (m() > 0) --k_;
        } else if ((ends("ed") || ends("ing")) && vowelInStem()) {
            k_ = j_;
            if (ends("at")) setTo("ate");
            else if (ends("bl")) setTo("ble");
            else if (ends("iz")) setTo("ize");
            else if (doubleC(k_)) {
                --k_;
                const char ch = b_[k_];
                if (ch == 'l' || ch == 's' || ch == 'z') ++k_;
            } else if (m() == 1 && cvc(k_)) {
                setTo("e");
            }
        }
    }

    void step1c() {
        if (ends("y") && vowelInStem()) b_[k_] = 'i';
    }

    void step2() {
        if (k_ < 1) return;
        switch (b_[k_ - 1]) {
            case 'a':
                if (ends("ational")) { r("ate"); break; }
                if (ends("tional")) { r("tion"); break; }
                break;
            case 'c':
                if (ends("enci")) { r("ence"); break; }
                if (ends("anci")) { r("ance"); break; }
                break;
            case 'e':
                if (ends("izer")) { r("ize"); break; }
                break;
            case 'l':
                if (ends("bli")) { r("ble"); break; }
                if (ends("alli")) { r("al"); break; }
                if (ends("entli")) { r("ent"); break; }
                if (ends("eli")) { r("e"); break; }
                if (ends("ousli")) { r("ous"); break; }
                break;
            case 'o':
                if (ends("ization")) { r("ize"); break; }
                if (ends("ation")) { r("ate"); break; }
                if (ends("ator")) { r("ate"); break; }
                break;
            case 's':
                if (ends("alism")) { r("al"); break; }
                if (ends("iveness")) { r("ive"); break; }
                if (ends("fulness")) { r("ful"); break; }
                if (ends("ousness")) { r("ous"); break; }
                break;
            case 't':
                if (ends("aliti")) { r("al"); break; }
                if (ends("iviti")) { r("ive"); break; }
                if (ends("biliti")) { r("ble"); break; }
                break;
            case 'g':
                if (ends("logi")) { r("log"); break; }
                break;
            default: break;
        }
    }

    void step3() {
        switch (b_[k_]) {
            case 'e':
                if (ends("icate")) { r("ic"); break; }
                if (ends("ative")) { r(""); break; }
                if (ends("alize")) { r("al"); break; }
                break;
            case 'i':
                if (ends("iciti")) { r("ic"); break; }
                break;
            case 'l':
                if (ends("ical")) { r("ic"); break; }
                if (ends("ful")) { r(""); break; }
                break;
            case 's':
                if (ends("ness")) { r(""); break; }
                break;
            default: break;
        }
    }

    void step4() {
        if (k_ < 1) return;
        switch (b_[k_ - 1]) {
            case 'a': if (ends("al")) break; return;
            case 'c': if (ends("ance") || ends("ence")) break; return;
            case 'e': if (ends("er")) break; return;
            case 'i': if (ends("ic")) break; return;
            case 'l': if (ends("able") || ends("ible")) break; return;
            case 'n': if (ends("ant") || ends("ement") || ends("ment") || ends("ent")) break; return;
            case 'o':
                if (ends("ion") && j_ >= 0 && (b_[j_] == 's' || b_[j_] == 't')) break;
                if (ends("ou")) break;
                return;
            case 's': if (ends("ism")) break; return;
            case 't': if (ends("ate") || ends("iti")) break; return;
            case 'u': if (ends("ous")) break; return;
            case 'v': if (ends("ive")) break; return;
            case 'z': if (ends("ize")) break; return;
            default: return;
        }
        if (m() > 1) k_ = j_;
    }

    void step5() {
        j_ = k_;
        if (b_[k_] == 'e') {
            const int a = m();
            if (a > 1 || (a == 1 && !cvc(k_ - 1))) --k_;
        }
        if (b_[k_] == 'l' && doubleC(k_) && m() > 1) --k_;
    }

    std::string b_;
    int k_;
    int j_ = 0;
};

}  // namespace

std::string porterStem(const std::string& word) {
    if (word.size() <= 2) return word;
    for (char c : word)
        if (c < 'a' || c > 'z') return word;
    return Porter(word).run();
}

}  // namespace lumen
