# Этап сборки: устанавливаем компилятор и средства разработки, затем собираем и проверяем программу.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make libpq-dev ca-certificates && rm -rf /var/lib/apt/lists/*
# Исходники берутся из папки orders; исключения перечислены в .dockerignore.
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release && cmake --build /build -j2 && ctest --test-dir /build --output-on-failure
# Этап запуска: оставляем готовую программу и библиотеку PostgreSQL без компилятора.
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libpq5 ca-certificates && rm -rf /var/lib/apt/lists/* && useradd --uid 10001 --create-home orders
COPY --from=build /build/orders /usr/local/bin/orders
# Сервис работает от отдельного пользователя контейнера.
USER orders
# Указываем порт приложения; его публикация на компьютере настраивается в Compose.
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/orders"]
