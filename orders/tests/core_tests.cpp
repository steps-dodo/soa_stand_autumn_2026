// Проверки расчётов и автомата защиты
#include "core.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>

// Если условие не выполнено, завершаем тест с понятным сообщением об ошибке
void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    try {
        // Проверяем обычный заказ
        Json order = {{"user_id", "u"}, {"items", Json::array({
            {{"sku", "x"}, {"qty", 2}, {"price_cents", 49900}}
        })}};
        check(calculateAmount(order) == 99800, "Incorrect amount");
        // Отрицательное, дробное, строковое и слишком большое значение отклонены
        for (Json invalid : {Json(-1), Json(1.5), Json("2"), Json(uint64_t(-1))}) {
            bool rejected = false;
            try { readInteger(invalid, 1); } catch (...) { rejected = true; }
            check(rejected, "Invalid number accepted");
        }
        // Проверяем переполнение
        order["items"][0]["price_cents"] = INT64_MAX;
        bool overflow = false;
        try { calculateAmount(order); } catch (...) { overflow = true; }
        check(overflow, "Overflow was not rejected");
        check(growingDelay(200, 0, 3000) == 200, "First retry delay");
        check(growingDelay(200, 30, 3000) == 3000, "Maximum retry delay");

        // Проверяем открытие защиты после ошибок и запрет запоздалому успеху закрыть её
        CircuitBreaker breaker;
        auto now = Clock::now();
        int stalePermission = breaker.beginRequest(now);
        for (int i = 0; i < 3; ++i) {
            int permission = breaker.beginRequest(now);
            check(permission >= 0, "Premature open");
            breaker.finishRequest(permission, false, 5000, now);
        }
        check(breaker.beginRequest(now) == -1, "Breaker must be open");
        breaker.finishRequest(stalePermission, true, 5000, now);
        check(breaker.beginRequest(now) == -1, "Stale success closed the breaker");

        // Перемещаем тестовое время к окончанию паузы; реального ожидания пяти секунд здесь нет
        now += std::chrono::seconds(5);
        std::atomic<int> allowed{0};
        std::atomic<int> probePermission{-1};
        std::vector<std::thread> threads;
        // Одновременно запрашиваем разрешение из двадцати потоков
        for (int i = 0; i < 20; ++i) {
            threads.emplace_back([&] {
                int permission = breaker.beginRequest(now);
                if (permission >= 0) {
                    ++allowed;
                    probePermission = permission;
                }
            });
        }
        for (auto& thread : threads) { thread.join(); }
        check(allowed == 1, "Only one recovery probe is allowed");
        breaker.finishRequest(probePermission, true, 5000, now);
        check(breaker.beginRequest(now) >= 0, "Recovery failed");

        // Проверяем точную границу короткой паузы
        CircuitBreaker responsive;
        for (int i = 0; i < 3; ++i) {
            responsive.finishRequest(responsive.beginRequest(now), false, 250, now);
        }
        check(responsive.beginRequest(now + std::chrono::milliseconds(249)) == -1,
              "Retry started before cooldown");
        check(responsive.beginRequest(now + std::chrono::milliseconds(250)) >= 0,
              "Short cooldown did not finish");
        std::cout << "PASS: amounts, invalid input, overflow, backoff, concurrent breaker\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
