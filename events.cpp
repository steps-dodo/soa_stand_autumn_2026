// Task 2: надёжная отправка событий и однократное начисление бонусов.
#include "service.hpp"
#include "httplib.h"
#include <iostream>
#include <random>

// Берём одно готовое событие. Блокировка не позволяет двум отправителям взять одну строку.
bool Service::publishNextEvent(Database& db) {
    db.query("BEGIN");
    try {
        Rows rows = db.query(
            "SELECT id, order_id, "
            "to_char(occurred_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), attempts "
            "FROM outbox WHERE sent = false AND next_attempt_at <= now() "
            "ORDER BY next_attempt_at LIMIT 1 FOR UPDATE SKIP LOCKED");
        if (rows.empty()) {
            db.query("COMMIT");
            return false;
        }

        // id, order_id и время читаются из базы: у всех повторов одинаковое содержимое.
        const std::string& id = rows[0][0];
        Json event = {{"id", id}, {"type", "order.paid"},
                      {"order_id", rows[0][1]}, {"occurred_at", rows[0][2]}};
        httplib::Client client(config.eventsUrl);
        client.set_connection_timeout(std::chrono::milliseconds(300));
        client.set_read_timeout(std::chrono::milliseconds(800));
        client.set_write_timeout(std::chrono::milliseconds(500));
        auto reply = client.Post("/events", event.dump(), "application/json");

        // Только 202 подтверждает приём. Отмечаем отправку ПОСЛЕ ответа шины.
        if (reply && reply->status == 202) {
            db.query("UPDATE outbox SET sent = true WHERE id = $1", {id});
        } else {
            // Таймаут не означает, что событие потерялось: повторяем с прежним id.
            static thread_local std::mt19937 random(std::random_device{}());
            int maximum = growingDelay(200, std::stoi(rows[0][3]), 3000);
            int delay = std::uniform_int_distribution<int>(maximum / 2, maximum)(random);
            db.query("UPDATE outbox SET attempts = LEAST(attempts + 1, 30), "
                     "next_attempt_at = now() + ($2::int * interval '1 millisecond') WHERE id = $1",
                     {id, std::to_string(delay)});
        }
        db.query("COMMIT");
        return true;
    } catch (...) {
        // При падении до COMMIT отметка отправки не сохранится. После запуска будет повтор.
        try { db.query("ROLLBACK"); } catch (...) {}
        throw;
    }
}

// Отдельный цикл не даёт сбоям шины остановить резервирование и оплату заказов.
void Service::eventLoop() {
    while (!stopping) {
        try {
            Database db(config.databaseUrl);
            while (!stopping) {
                if (!publishNextEvent(db)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "Event worker error: " << error.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

// Две записи делаем одной транзакцией: запоминаем событие и увеличиваем баланс.
void Service::acceptDelivery(const Json& input) {
    Delivery delivery = readDelivery(input);
    Database db(config.databaseUrl);
    db.query("BEGIN");
    try {
        // RETURNING вернёт строку только для нового события. Дубликат даст пустой результат.
        // При одновременных копиях уникальный ключ заставит PostgreSQL дождаться первой транзакции.
        Rows inserted = db.query(
            "INSERT INTO inbox(event_id, user_id, bonus_points) VALUES($1, $2, $3) "
            "ON CONFLICT(event_id) DO NOTHING RETURNING event_id",
            {delivery.id, delivery.userId, std::to_string(delivery.points)});
        if (!inserted.empty()) {
            // Сложение выполняет сама база под блокировкой строки: параллельные начисления не теряются.
            db.query("INSERT INTO bonuses(user_id, points) VALUES($1, $2) "
                     "ON CONFLICT(user_id) DO UPDATE SET points = bonuses.points + EXCLUDED.points",
                     {delivery.userId, std::to_string(delivery.points)});
        }
        // На дубликат тоже отвечаем успехом, но только после завершения транзакции.
        db.query("COMMIT");
    } catch (...) {
        // Ошибка начисления откатывает и запись inbox, поэтому повтор сможет обработать событие заново.
        try { db.query("ROLLBACK"); } catch (...) {}
        throw;
    }
}

Json Service::getBonus(const std::string& userId) {
    Database db(config.databaseUrl);
    Rows rows = db.query("SELECT points FROM bonuses WHERE user_id = $1", {userId});
    // Отсутствующий покупатель — обычный случай: баланс нулевой, создавать строку не нужно.
    int64_t points = rows.empty() ? 0 : std::stoll(rows[0][0]);
    return {{"points", points}};
}
