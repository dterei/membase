#include "config.h"
#include <cassert>
#include <sstream>
#include <cmath>
#include <functional>
#include <algorithm>

#include "histo.hh"

class PopulatedSamples {
public:
    PopulatedSamples(std::ostream &stream) : s(stream) { }

    void operator() (const HistogramBin<int>* b) {
        if (b->count() > 0) {
            s << *b << "; ";
        }
    }
    std::ostream &s;
};

static void test_basic() {
    GrowingWidthGenerator<int> gen(0, 10, M_E);
    Histogram<int> histo(gen, 10);
    histo.add(3, 1);
    histo.add(-3, 15);
    histo.add(84477242, 11);

    // Verify the stream stuff works.
    std::stringstream s;
    s << histo;
    std::string expected("{Histogram: [-2147483648, 0) = 15, [0, 10) = 1, "
                         "[10, 37) = 0, [37, 110) = 0, [110, 310) = 0, "
                         "[310, 855) = 0, [855, 2339) = 0, [2339, 6373) = 0, "
                         "[6373, 17339) = 0, [17339, 47148) = 0, "
                         "[47148, 128178) = 0, [128178, 2147483647) = 11}");
    assert(s.str() == expected);

    std::stringstream s2;
    PopulatedSamples ps(s2);
    std::for_each(histo.begin(), histo.end(), ps);
    expected = "[-2147483648, 0) = 15; [0, 10) = 1; [128178, 2147483647) = 11; ";
    assert(s2.str() == expected);
    assert(27 == histo.total());

    // I haven't set a 4, but there should be something in that bin.
    assert(1 == histo.getBin(4)->count());

    histo.reset();
    assert(0 == histo.total());
}

static void test_fixed_input() {
    std::vector<int> figinput;
    figinput.push_back(1);
    figinput.push_back(10);
    figinput.push_back(100);
    figinput.push_back(1000);
    figinput.push_back(10000);
    FixedInputGenerator<int> fig(figinput);
    Histogram<int> histo(fig, 4);

    std::string expected("{Histogram: [-2147483648, 1) = 0, "
                         "[1, 10) = 0, [10, 100) = 0, [100, 1000) = 0, "
                         "[1000, 10000) = 0, [10000, 2147483647) = 0}");
    std::stringstream s;
    s << histo;
    assert(s.str() == expected);
}

static void test_exponential() {
    ExponentialGenerator<int> gen(0, 10.0);
    Histogram<int> histo(gen, 5);
    std::string expected("{Histogram: [-2147483648, 1) = 0, [1, 10) = 0, "
                         "[10, 100) = 0, [100, 1000) = 0, [1000, 10000) = 0, "
                         "[10000, 100000) = 0, [100000, 2147483647) = 0}");
    std::stringstream s;
    s << histo;
    assert(s.str() == expected);
}

static void test_complete_range() {
    GrowingWidthGenerator<uint16_t> gen(0, 10, M_E);
    Histogram<uint16_t> histo(gen, 10);
    uint16_t i(0);
    do {
        histo.add(i);
        ++i;
    } while (i != 0);
}

int main() {
    test_basic();
    test_fixed_input();
    test_exponential();
    test_complete_range();
    return 0;
}
