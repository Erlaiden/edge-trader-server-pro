#include "train_logic.h"
#include "json.hpp"

// Этот инклюд только прокидывает видимость правильных имён.
// ИСТОРИЧЕСКИ код в местах include "train.inc.cpp" вызывал run_train_pro_and_save без префикса.
// Здесь явно экспортируем нужные using, чтобы он резолвился в etai::
using nlohmann::json;
using etai::run_train_pro_and_save;
