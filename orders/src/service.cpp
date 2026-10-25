#include "service.hpp"
#include "httplib.h"
#include <iostream>
#include <random>

namespace {
struct Reply {
    int status = 0; // 0 — HTTP-ответ вообще не получен
    Json body;
};

// Отправляем один HTTP-запрос
Reply sendRequest(const std::string& address, const std::string& method,
                  const std::string& path, const Json& body = nullptr,
                  const std::string& key = "", int timeoutMs = 800) {
    httplib::Client client(address);
    // Ограничиваем соединение, чтение ответа и запись запроса, чтобы работник не ждал бесконечно
    client.set_connection_timeout(std::chrono::milliseconds(300));
    client.set_read_timeout(std::chrono::milliseconds(timeoutMs));
    client.set_write_timeout(std::chrono::milliseconds(500));
    httplib::Headers headers;
    // Для оплаты передаём постоянный ключ операции
    if (!key.empty()) {
        headers.emplace("Idempotency-Key", key);
    }

    httplib::Result result;
    // Выбираем HTTP-метод
    if (method == "GET") {
        result = client.Get(path, headers);
    } else if (method == "DELETE") {
        result = client.Delete(path, headers);
    } else {
        result = client.Post(path, headers, body.dump(), "application/json");
    }
    // Ответ не получен
    if (!result) {
        return {};
    }
    // Разбираем тело без исключения на неправильном JSON
    Json parsed = Json::parse(result->body, nullptr, false);
    if (parsed.is_discarded()) {
        parsed = nullptr;
    }
    return {result->status, parsed};
}

// Считаем оплату подтверждённой только при совпадении заказа, суммы, валюты и статуса списания
bool matchesPayment(const Json& payment, const Order& order) {
    if (!payment.is_object()) {
        return false;
    }
    return payment.value("order_id", "") == order.id
        && payment.value("status", "") == "captured"
        && payment.value("currency", "") == "RUB"
        && payment.contains("amount_cents")
        && readInteger(payment.at("amount_cents"), 1) == order.amount;
}

// После неизвестного исхода отдельно спрашиваем банк, есть ли успешное списание по этому заказу
bool findPayment(const std::string& paymentUrl, const Order& order) {
    Reply reply = sendRequest(paymentUrl, "GET", "/payments?order_id=" + order.id);
    if (reply.status != 200 || !reply.body.is_array()) {
        return false;
    }
    for (const Json& payment : reply.body) {
        if (matchesPayment(payment, order)) {
            return true;
        }
    }
    // Отсутствие списания в ответе ещё НЕ является отказом банка
    return false;
}

// Защищаем вывод от одновременной записи
void printMessage(const std::string& text) {
    static std::mutex outputMutex;
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cerr << text << std::endl;
}
}

// Сохраняем настройки внутри сервиса, чтобы все его методы использовали одни и те же адреса
Service::Service(const Config& settings) : config(settings) {}

// При обычном завершении просим работников остановиться и ждём окончания их текущих действий
Service::~Service() {
    stopping = true;
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

// Создаём фиксированное число работников, а не новый поток для каждого поступившего заказа
void Service::start() {
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back(&Service::workerLoop, this);
    }
}

// Сначала проверяем заказ и сохраняем его
Json Service::createOrder(const Json& input) {
    int64_t amount = calculateAmount(input);
    Database db(config.databaseUrl);
    // Данные передаём параметрами, отдельно от SQL
    Rows rows = db.query(
        "INSERT INTO orders(user_id, amount_cents, items) "
        "VALUES($1, $2, $3::jsonb) RETURNING id",
        {input.at("user_id").get<std::string>(), std::to_string(amount),
         input.at("items").dump()});
    // Запросы к складу и банку сделает работник
    return {{"id", rows[0][0]}, {"status", "pending"}};
}

// Возвращаем сохранённый статус и сумму
Json Service::getOrder(const std::string& id) {
    Database db(config.databaseUrl);
    Rows rows = db.query("SELECT id, status, amount_cents FROM orders WHERE id = $1", {id});
    if (rows.empty()) {
        return nullptr;
    }
    // В первой строке результата столбцы идут в порядке SELECT: номер, статус, сумма
    return {{"id", rows[0][0]}, {"status", rows[0][1]},
            {"amount_cents", std::stoll(rows[0][2])}};
}

// После временной ошибки назначаем следующую попытку, сохраняя заказ в pending
void Service::retryLater(Database& db, const Order& order) {
    // У каждого потока собственный генератор случайных чисел, поэтому отдельная блокировка ему не нужна
    static thread_local std::mt19937 random(std::random_device{}());
    // Случайный разброс помогает заказам повторяться в разное время
    int maximum = growingDelay(200, order.attempts, 3000);
    int delay = std::uniform_int_distribution<int>(maximum / 2, maximum)(random);
    // Ограничиваем только значение счётчика
    db.query("UPDATE orders SET attempts = LEAST(attempts + 1, 30), "
             "next_attempt_at = now() + ($2::int * interval '1 millisecond') WHERE id = $1",
             {order.id, std::to_string(delay)});
}

// Первый этап: просим склад удержать товары за этим заказом. До подтверждения резерва
void Service::reserveOrder(Database& db, const Order& order) {
    // Складу нужны только обозначение товара и количество, ф цену используем при расчёте суммы для банка
    Json items = Json::array();
    for (const Json& item : order.items) {
        items.push_back({{"sku", item.at("sku")}, {"qty", item.at("qty")}});
    }
    Reply reply = sendRequest(config.inventoryUrl, "POST", "/reservations",
                              {{"order_id", order.id}, {"items", items}});
    // Подходит как первое успешное резервирование, так и успешный повтор той же операции
    if (reply.status == 200 || reply.status == 201) {
        db.query("UPDATE orders SET stage = 'pay', attempts = 0, "
                 "next_attempt_at = now() WHERE id = $1", {order.id});
    } else {
        retryLater(db, order);
    }
}

// Этап оплаты: проверяем доступность банка, отправляем платёж и выясняем окончательный результат
void Service::payOrder(Database& db, const Order& order) {
    int permission = paymentBreaker.beginRequest();
    // Во время паузы автомата не вызываем банк. Откладываем только проверку этого заказа в своей базе
    if (permission == -1) {
        db.query("UPDATE orders SET next_attempt_at = now() + interval '250 milliseconds' "
                 "WHERE id = $1", {order.id});
        return;
    }

    Reply reply;
    bool paid = false;
    try {
        // Постепенно увеличиваем ожидание
        int timeout = growingDelay(900, order.attempts, 12000);
        // Ключ строится из сохранённого номера заказа и не меняется при повторах
        reply = sendRequest(config.paymentUrl, "POST", "/payments",
                            {{"order_id", order.id}, {"amount_cents", order.amount},
                             {"currency", "RUB"}}, "pay:" + order.id, timeout);
        if (reply.status == 200 || reply.status == 201) {
            paid = matchesPayment(reply.body, order);
        }
        // Если нет подтверждения и нет явного отказа, ищем списание отдельным запросом
        if (!paid && reply.status != 402) {
            paid = findPayment(config.paymentUrl, order);
        }
    } catch (...) {
        // Даже при исключении завершаем разрешение автомата, иначе он может навсегда ждать пробный запрос
        paymentBreaker.finishRequest(permission, false);
        throw;
    }

    // Банковский отказ 402 означает, что банк доступен и принял решение
    bool healthy = paid || reply.status == 402;
    int pauseMs = 5000;
    if (reply.status == 500) {
        pauseMs = 250;
    }
    paymentBreaker.finishRequest(permission, healthy, pauseMs);

    // Подтверждённая оплата завершает заказ
    if (paid) {
        db.query("UPDATE orders SET status = 'paid', stage = 'done' WHERE id = $1", {order.id});
    } else if (reply.status == 402) {
        db.query("UPDATE orders SET stage = 'release', attempts = 0, "
                 "next_attempt_at = now() WHERE id = $1", {order.id});
    } else {
        retryLater(db, order);
    }
}

// После 402 снимаем резерв
void Service::releaseOrder(Database& db, const Order& order) {
    Reply reply = sendRequest(config.inventoryUrl, "DELETE", "/reservations/" + order.id);
    if (reply.status == 204) {
        db.query("UPDATE orders SET status = 'rejected', stage = 'done' WHERE id = $1", {order.id});
    } else {
        retryLater(db, order);
    }
}

// Один вызов выполняет один этап одного заказа
bool Service::processNextOrder(Database& db) {
    db.query("BEGIN");
    try {
        // Блокируем одну строку до COMMIT
        Rows rows = db.query(
            "SELECT id, stage, amount_cents, items, attempts FROM orders "
            "WHERE status = 'pending' AND next_attempt_at <= now() "
            "ORDER BY next_attempt_at, created_at LIMIT 1 FOR UPDATE SKIP LOCKED");
        // Готовых свободных заказов сейчас нет. Закрываем транзакцию и сообщаем работнику, что можно подождать
        if (rows.empty()) {
            db.query("COMMIT");
            return false;
        }
        // Читаем первую строку: столбцы 0–4 — номер, этап, сумма, позиции и число попыток.
        Order order{rows[0][0], rows[0][1], std::stoll(rows[0][2]),
                    Json::parse(rows[0][3]), std::stoi(rows[0][4])};
        // Выбираем действие по сохранённому этапу
        if (order.stage == "reserve") {
            reserveOrder(db, order);
        } else if (order.stage == "pay") {
            payOrder(db, order);
        } else if (order.stage == "release") {
            releaseOrder(db, order);
        } else {
            throw std::runtime_error("Unexpected order stage");
        }
        // Сохраняем изменения этапа и освобождаем блокировку строки для остальных работников
        db.query("COMMIT");
        return true;
    } catch (...) {
        // Не оставляем незавершённую транзакцию после ошибки
        try { db.query("ROLLBACK"); } catch (...) {}
        throw;
    }
}

// Каждый работник использует своё подключение к базе и последовательно берёт готовые шаги заказов
void Service::workerLoop() {
    while (!stopping) {
        try {
            Database db(config.databaseUrl);
            while (!stopping) {
                // При пустой очереди делаем короткую паузу чтобы не нагружать базу непрерывными запросами
                if (!processNextOrder(db)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }
        } catch (const std::exception& error) {
            // При ошибке подключения или обработки выводим причину
            printMessage(std::string("Worker error: ") + error.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}
