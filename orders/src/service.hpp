// Типы данных и интерфейс сервиса заказов. Реализация методов находится в service.cpp.
#pragma once
#include "core.hpp"
#include "database.hpp"
#include <atomic>
#include <thread>
#include <vector>

// Настройки подключения: своя база, внешний банк и склад. Шина событий для Task 1 не нужна.
struct Config {
    std::string databaseUrl;
    std::string paymentUrl;
    std::string inventoryUrl;
    int port = 8080;
};

// Данные выбранного заказа. Внутренний этап и число попыток позволяют продолжить обработку.
struct Order {
    std::string id;
    std::string stage;
    int64_t amount;
    Json items;
    int attempts;
};

// Сервис принимает заказы через публичные методы, а внутренние этапы выполняет в фоне.
class Service {
public:
    explicit Service(const Config& settings);
    ~Service();
    void start();
    Json createOrder(const Json& input);
    Json getOrder(const std::string& id);

private:
    Config config;
    // Флаг читается и меняется разными потоками, поэтому используем атомарный тип.
    std::atomic<bool> stopping{false};
    std::vector<std::thread> workers;
    // Один общий автомат защиты банка для всех работников ограничивает обращения во время сбоя.
    CircuitBreaker paymentBreaker;

    // Цикл работника и выбор следующего заказа, которому уже пора продолжать обработку.
    void workerLoop();
    bool processNextOrder(Database& db);
    // Отдельный метод для каждого шага: зарезервировать, оплатить, снять резерв, назначить повтор.
    void reserveOrder(Database& db, const Order& order);
    void payOrder(Database& db, const Order& order);
    void releaseOrder(Database& db, const Order& order);
    void retryLater(Database& db, const Order& order);
};
