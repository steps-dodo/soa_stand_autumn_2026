// Проверка входных данных, расчёт суммы и общий автомат ограничения запросов к банку.
#pragma once
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

using Json = nlohmann::json;
// Для пауз используем монотонные часы: перевод системного времени не должен менять длительность ожидания.
using Clock = std::chrono::steady_clock;

// Проверяем обязательное текстовое поле: оно должно быть строкой и не должно быть пустым.
inline std::string readText(const Json& value) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        throw std::invalid_argument("Expected a nonempty string");
    }
    return value.get<std::string>();
}

// Принимаем только целое число, которое помещается в тип и не меньше указанной границы.
inline int64_t readInteger(const Json& value, int64_t minimum) {
    if (!value.is_number_integer()) {
        throw std::invalid_argument("Expected an integer");
    }
    // Большое беззнаковое JSON-число нельзя без проверки переводить в знаковое число из 64 бит.
    if (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t(INT64_MAX)) {
        throw std::invalid_argument("Integer is too large");
    }
    int64_t number = value.get<int64_t>();
    if (number < minimum) {
        throw std::invalid_argument("Integer is below the minimum");
    }
    return number;
}

// Считаем сумму всех позиций в целых копейках, чтобы избежать ошибок округления дробных чисел.
inline int64_t calculateAmount(const Json& order) {
    readText(order.at("user_id"));
    const Json& items = order.at("items");
    if (!items.is_array() || items.empty()) {
        throw std::invalid_argument("Items must be a nonempty array");
    }

    // До обхода позиций сумма нулевая; каждая позиция добавляет количество, умноженное на цену.
    int64_t total = 0;
    for (const Json& item : items) {
        readText(item.at("sku"));
        // Количество должно быть положительным. Нулевая цена отдельной позиции допускается.
        int64_t quantity = readInteger(item.at("qty"), 1);
        int64_t price = readInteger(item.at("price_cents"), 0);
        // Проверяем переполнение ДО умножения и сложения.
        if (price != 0 && quantity > (INT64_MAX - total) / price) {
            throw std::invalid_argument("Order amount is too large");
        }
        total += quantity * price;
    }
    // Полностью нулевую сумму не отправляем в банк: по его контракту платёж должен быть положительным.
    if (total == 0) {
        throw std::invalid_argument("Payment requires a positive amount");
    }
    return total;
}

// Увеличиваем задержку вдвое с каждой попыткой, но не выше переданного предела.
inline int growingDelay(int start, int attempts, int maximum) {
    int delay = start;
    // Обычное удвоение вместо битовых операций.
    for (int i = 0; i < attempts && delay < maximum; ++i) {
        delay = std::min(delay * 2, maximum);
    }
    return delay;
}

// Общий для четырёх работников автомат защиты банка.
class CircuitBreaker {
public:
    // -1: запрос пока запрещён. Остальное: номер поколения разрешения.
    // Под блокировкой решаем, можно ли начать запрос. Нулевое поколение допустимо; запрет обозначен минус единицей.
    int beginRequest(Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex);
        if (state == Open) {
            if (now < retryAt) {
                return -1;
            }
            // Срок паузы истёк: разрешаем одному работнику проверить восстановление банка.
            state = HalfOpen;
            return generation;
        }
        if (state == HalfOpen) {
            return -1; // Пробный запрос уже делает другой работник.
        }
        return generation;
    }

    // Обновляем состояние защиты по результату запроса, используя выданный ему номер поколения.
    void finishRequest(int requestGeneration, bool healthy, int pauseMs = 5000,
                       Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex);
        // Старый ответ не должен закрыть автомат, который уже заново открылся из-за других ошибок.
        if (requestGeneration != generation) {
            return; // Запоздалый ответ относится к старому состоянию банка.
        }
        // Успешная связь с банком завершает серию ошибок и возвращает обычный режим.
        if (healthy) {
            failures = 0;
            state = Closed;
            return;
        }
        ++failures;
        // Открываем автомат после трёх ошибок или сразу после неудачной пробы восстановления.
        if (state == HalfOpen || failures >= 3) {
            state = Open;
            retryAt = now + std::chrono::milliseconds(pauseMs);
            // Теперь результаты ранее выданных разрешений считаются устаревшими для состояния автомата.
            ++generation;
        }
    }

private:
    // Три состояния: обычная работа, запрет запросов на время паузы и одна проба восстановления.
    enum State { Closed, Open, HalfOpen };
    std::mutex mutex;
    State state = Closed;
    int failures = 0;
    int generation = 0;
    Clock::time_point retryAt{};
};
