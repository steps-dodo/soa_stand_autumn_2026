#include "service.hpp"
#include "httplib.h"
#include <cstdlib>
#include <iostream>

// Получаем адрес сервиса из окружения
std::string readEnvironment(const char* name) {
    const char* value = std::getenv(name);
    // Сначала проверяем указатель, затем пустую строку
    if (value == nullptr || value[0] == '\0') {
        throw std::runtime_error(std::string("Missing variable: ") + name);
    }
    return value;
}

// Записываем HTTP-код и JSON-тело в ответ, который затем отправит библиотека сервера
void sendJson(httplib::Response& response, int status, const Json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json; charset=utf-8");
}

int main() {
    try {
        // Читаем настройки
        Config config;
        config.databaseUrl = readEnvironment("DATABASE_URL");
        config.paymentUrl = readEnvironment("PAYMENT_URL");
        config.inventoryUrl = readEnvironment("INVENTORY_URL");
        config.eventsUrl = readEnvironment("EVENTS_URL");
        // Если порт не передали, используем начальное значение 8080 из структуры настроек
        if (std::getenv("PORT") != nullptr) {
            config.port = std::stoi(readEnvironment("PORT"));
        }
        if (config.port < 1 || config.port > 65535) {
            throw std::runtime_error("Invalid port");
        }

        // Повторяем подключение и создание таблицы
        bool ready = false;
        for (int attempt = 0; attempt < 60 && !ready; ++attempt) {
            try {
                Database db(config.databaseUrl);
                db.initialize();
                ready = true;
            } catch (const std::exception& error) {
                std::cerr << "Waiting for database: " << error.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        if (!ready) {
            throw std::runtime_error("Database did not become ready");
        }

        // Четыре работника оформляют заказы, ещё два отправляют события.
        Service service(config);
        service.start();
        httplib::Server server;
        // Отдельные восемь потоков HTTP-сервера принимают запросы покупателей, не ожидая оплаты
        server.new_task_queue = [] { return new httplib::ThreadPool(8); };
        // Свободное keep-alive соединение не должно занимать поток сервера
        server.set_keep_alive_max_count(1);
        // Ограничиваем размер тела и ожидание данных от HTTP-клиента
        server.set_payload_max_length(1024 * 1024);
        server.set_read_timeout(5, 0);
        server.set_write_timeout(5, 0);

        // Проверяем готовность приложения и базы
        server.Get("/healthz", [&](const httplib::Request&, httplib::Response& response) {
            try {
                Database db(config.databaseUrl);
                db.query("SELECT 1");
                sendJson(response, 200, {{"status", "ok"}});
            } catch (const std::exception&) {
                sendJson(response, 503, {{"error", "database_unavailable"}});
            }
        });

        // Принимаем заказ: разбираем JSON, сохраняем его и сразу отвечаем 201 с номером и pending
        server.Post("/orders", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                Json input = Json::parse(request.body);
                sendJson(response, 201, service.createOrder(input));
            // Ошибочный JSON или неверные поля — ошибка запроса клиента, поэтому возвращаем 400
            } catch (const Json::exception&) {
                sendJson(response, 400, {{"error", "bad_request"}});
            } catch (const std::invalid_argument& error) {
                sendJson(response, 400, {{"error", error.what()}});
            } catch (const std::exception& error) {
                std::cerr << error.what() << std::endl;
                sendJson(response, 503, {{"error", "database_unavailable"}});
            }
        });

        // Читаем текущий статус заказа
        server.Get(R"(/orders/([^/]+))", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                Json order = service.getOrder(request.matches[1]);
                // Пустой результат поиска означает, что такого заказа нет
                if (order.is_null()) {
                    sendJson(response, 404, {{"error", "not_found"}});
                } else {
                    sendJson(response, 200, order);
                }
            } catch (const std::exception& error) {
                std::cerr << error.what() << std::endl;
                sendJson(response, 503, {{"error", "database_unavailable"}});
            }
        });

        // Подтверждаем доставку только после сохранения события и начисления бонусов в базе.
        server.Post("/webhooks/delivery", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                service.acceptDelivery(Json::parse(request.body));
                sendJson(response, 200, {{"status", "ok"}});
            } catch (const Json::exception&) {
                sendJson(response, 400, {{"error", "bad_request"}});
            } catch (const std::invalid_argument& error) {
                sendJson(response, 400, {{"error", error.what()}});
            } catch (const std::exception& error) {
                std::cerr << error.what() << std::endl;
                // При ошибке базы не подтверждаем обработку: шина должна повторить доставку.
                sendJson(response, 503, {{"error", "database_unavailable"}});
            }
        });

        server.Get(R"(/users/([^/]+)/bonus)", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                // Первая группа регулярного выражения содержит user_id из адреса.
                sendJson(response, 200, service.getBonus(request.matches[1]));
            } catch (const std::exception& error) {
                std::cerr << error.what() << std::endl;
                sendJson(response, 503, {{"error", "database_unavailable"}});
            }
        });

        std::cout << "Orders Task 1 + Task 2: port " << config.port << std::endl;
        // Запускаем приём запросов на всех IPv4-интерфейсах
        if (!server.listen("0.0.0.0", config.port)) {
            throw std::runtime_error("Cannot listen on the selected port");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        // Ошибка запуска: ненулевой код сообщает оболочке или контейнеру о неудаче
        return 1;
    }
    return 0;
}
