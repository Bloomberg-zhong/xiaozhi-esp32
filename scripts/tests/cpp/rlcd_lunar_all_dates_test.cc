#include <cassert>
#include <cstdio>
#include "dashboard_model.h"

int main() {
    for (int year = 1901; year <= 2100; ++year) {
        for (int month = 1; month <= 12; ++month) {
            for (int day = 1; day <= rlcd_dashboard::DaysInMonth(year, month); ++day) {
                auto lunar = rlcd_dashboard::GregorianToLunar(year, month, day);
                assert(lunar);
                std::printf("%d,%d,%d,%d,%d,%d,%d\n", year, month, day, lunar->year, lunar->month,
                            lunar->day, lunar->leap ? 1 : 0);
            }
        }
    }
}
