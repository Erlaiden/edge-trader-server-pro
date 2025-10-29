#pragma once
#include "httplib.h"

// Регистрирует HTTP-роуты инференса.
// Реализация — в src/routes/infer.cpp (который уже содержит логику и инклюды .inc при необходимости).
void register_infer_routes(httplib::Server& srv);
