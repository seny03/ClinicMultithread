#include <chrono> // Подключаем заголовок для работы с временем (chrono)
#include <cstdio> // Подключаем стандартную библиотеку ввода-вывода C (printf, fprintf)
#include <cstdlib> // Подключаем стандартную библиотеку C (atoi, rand и др.)
#include <cstring> // Подключаем библиотеку для работы со строками C (strcmp)
#include <fstream> // Подключаем библиотеку для работы с файлами (ifstream, ofstream)
#include <iostream> // Подключаем стандартную библиотеку ввода-вывода C++ (cin, cout)
#include <pthread.h> // Подключаем библиотеку для работы с потоками POSIX (pthread_*)
#include <queue> // Подключаем контейнер очередь (std::queue)
#include <random> // Подключаем библиотеку для работы со случайными числами (std::random)
#include <string> // Подключаем класс std::string

#if _WIN32                // Если компиляция под Windows
#include <windows.h>      // Подключаем Windows.h для Sleep
#define sleep(x) Sleep(x) // Определяем sleep как Sleep
#else                     // Иначе (Linux/UNIX)
#include <stdarg.h> // Подключаем для работы с variadic аргументами (va_list)
#include <unistd.h> // Подключаем для работы с UNIX-функциями (usleep)
#define sleep(x) usleep(1000L * x) // Определяем sleep(x) через usleep
#endif

// Структура пациента
enum SpecialistType {
  NONE = -1,
  DENTIST = 0,
  SURGEON = 1,
  THERAPIST = 2
}; // Перечисление типов специалистов

struct Patient {
  int id; // Идентификатор пациента
  SpecialistType
      specialist_type; // Тип специалиста, к которому пациент направлен
  pthread_cond_t treated =
      PTHREAD_COND_INITIALIZER; // Условная переменная для ожидания лечения
  pthread_mutex_t patientLock =
      PTHREAD_MUTEX_INITIALIZER; // Мьютекс для синхронизации состояния пациента
};

// Глобальные переменные (для простоты)
int N = 5;      // Число пациентов
int t_d = 1000; // Время приема дежурного врача (мс)
int t_s = 2000; // Время приема специалиста (мс)
std::string output_filename =
    "data/clinic_log.txt"; // Имя файла для вывода логов
bool from_file = false; // Флаг чтения параметров из файла
std::string config_filename; // Имя файла конфигурации

// Потоки
pthread_t *patients; // Массив потоков пациентов
pthread_t duty_docs[2]; // Массив потоков дежурных врачей (2 врача)
pthread_t specialists[3]; // Массив потоков специалистов (3 специалиста)

// Очередь к дежурным
std::queue<Patient *> commonQueue; // Очередь пациентов к дежурным врачам
pthread_mutex_t commonQueueLock =
    PTHREAD_MUTEX_INITIALIZER; // Мьютекс для очереди дежурных
pthread_cond_t commonQueueNotEmpty =
    PTHREAD_COND_INITIALIZER; // Условная переменная для оповещения о новых
                              // пациентах в очереди дежурных

// Очереди к специалистам: стоматолог(0), хирург(1), терапевт(2)
std::queue<Patient *> specialistQueue[3]; // Три очереди для трех специалистов
pthread_mutex_t specialistLock[3]; // Мьютексы для каждой очереди специалистов
pthread_cond_t
    specialistNotEmpty[3]; // Условные переменные для очередей специалистов

pthread_mutex_t consoleLogLock =
    PTHREAD_MUTEX_INITIALIZER; // Мьютекс для логирования в консоль
pthread_mutex_t fileLogLock =
    PTHREAD_MUTEX_INITIALIZER; // Мьютекс для логирования в файл

// Новые глобальные переменные для подсчета направленных к специалисту пациентов
int patientsToSpecialist = 0; // Счетчик направленных к специалисту пациентов
pthread_mutex_t patientsToSpecialistLock =
    PTHREAD_MUTEX_INITIALIZER; // Мьютекс для доступа к этому счетчику

// Файл вывода
FILE *log_file = NULL; // Указатель на файл логов
auto program_start =
    std::chrono::high_resolution_clock::now(); // Время старта программы (для
                                               // таймеров)

// Генератор случайных чисел
std::mt19937
    rng(42); // Стандартный генератор случайных чисел с фиксированным сидом
std::uniform_int_distribution<int>
    specialist_dist(0, 2); // Распределение для выбора специалиста

// Функция для получения времени с момента старта программы
std::string get_time_since_start() {
  auto now = std::chrono::high_resolution_clock::now(); // Берем текущее время
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - program_start)
          .count(); // Вычисляем прошедшее время в мс
  int minutes = elapsed / 60000;       // Переводим в минуты
  int seconds = (elapsed / 1000) % 60; // Остаток в секундах
  int milliseconds = elapsed % 1000;   // Миллисекунды
  char buffer[30]; // Буфер для форматирования строки времени
  sprintf(buffer, "[%02d:%02d:%03d]", minutes, seconds,
          milliseconds); // Записываем форматированную строку времени
  return std::string(buffer); // Возвращаем строку с временем
}

// Функция логирования
void log_event(const char *fmt, ...) {
  va_list args; // Список аргументов
  va_start(args, fmt); // Инициализируем список аргументов

  va_list args2; // Второй список аргументов для логирования в файл
  va_copy(args2, args); // Копируем аргументы

  std::string time_str =
      get_time_since_start(); // Получаем текущее время с начала программы
  // Вывод в консоль
  pthread_mutex_lock(&consoleLogLock); // Захватываем мьютекс консоли
  printf("%s ", time_str.c_str()); // Печатаем время
  vprintf(fmt, args); // Печатаем само сообщение
  pthread_mutex_unlock(&consoleLogLock); // Освобождаем мьютекс консоли

  // Вывод в файл
  if (log_file) {                     // Если файл открыт
    pthread_mutex_lock(&fileLogLock); // Захватываем мьютекс файла
    fprintf(log_file, "%s ", time_str.c_str()); // Печатаем время в файл
    vfprintf(log_file, fmt, args2); // Печатаем сообщение в файл
    pthread_mutex_unlock(&fileLogLock); // Освобождаем мьютекс файла
    va_end(args2); // Завершаем работу со списком аргументов для файла
  }
  va_end(args); // Завершаем работу с основным списком аргументов
}

// Поток пациента
void *patient_thread(void *arg) {
  int pid = *(int *)arg; // Извлекаем id пациента из аргумента
  delete (int *)arg; // Освобождаем память под id

  Patient *p = new Patient(); // Создаем новый объект пациента
  p->id = pid;                // Присваиваем ему id
  p->specialist_type = NONE; // По умолчанию без специалиста

  pthread_mutex_lock(&commonQueueLock); // Захватываем мьютекс очереди дежурных
  commonQueue.push(p); // Добавляем пациента в очередь
  log_event("Пациент P%d встал в очередь к дежурным\n",
            p->id); // Логируем событие
  pthread_cond_signal(
      &commonQueueNotEmpty); // Сигнализируем, что очередь теперь не пуста
  pthread_mutex_unlock(
      &commonQueueLock); // Освобождаем мьютекс очереди дежурных

  // Ждем, пока пациент будет вылечен
  pthread_mutex_lock(&p->patientLock); // Захватываем мьютекс пациента
  pthread_cond_wait(&p->treated,
                    &p->patientLock); // Ждем сигнала о том, что пациент вылечен
  pthread_mutex_unlock(&p->patientLock); // Освобождаем мьютекс пациента

  log_event("Пациент P%d полностью вылечен и пошел домой\n",
            p->id); // Логируем событие вылеченного пациента
  delete p;    // Освобождаем память под пациента
  return NULL; // Завершаем поток пациента
}

// Поток дежурного врача
void *duty_doctor_thread(void *arg) {
  int did = *(int *)arg; // Извлекаем id дежурного врача
  delete (int *)arg;     // Освобождаем память под id

  while (true) { // Бесконечный цикл (до выхода из него)
    pthread_mutex_lock(
        &commonQueueLock); // Захватываем мьютекс очереди дежурных

    while (commonQueue.empty()) { // Пока очередь пуста
      // Проверяем, не обработаны ли все пациенты
      pthread_mutex_lock(
          &patientsToSpecialistLock); // Захватываем мьютекс счетчика
      bool all_sent =
          (patientsToSpecialist == N); // Проверяем, обработаны ли все пациенты
      pthread_mutex_unlock(
          &patientsToSpecialistLock); // Освобождаем мьютекс счетчика

      if (all_sent) { // Если все пациенты уже были направлены к специалистам
        pthread_mutex_unlock(&commonQueueLock); // Освобождаем мьютекс очереди
        goto end_duty; // Переходим к концу функции (завершение врача)
      }

      pthread_cond_wait(
          &commonQueueNotEmpty,
          &commonQueueLock); // Ждем появления нового пациента в очереди
    }

    // Здесь очередь не пуста, берем пациента
    Patient *p = commonQueue.front(); // Берем пациента из очереди
    commonQueue.pop();                // Удаляем из очереди
    pthread_mutex_unlock(
        &commonQueueLock); // Освобождаем мьютекс очереди дежурных

    // Принимаем пациента
    log_event("Дежурный D%d принял пациента P%d\n", did,
              p->id); // Логируем событие приема пациента
    sleep(t_d);       // Имитируем время приема

    // Определяем специалиста
    p->specialist_type = static_cast<SpecialistType>(
        specialist_dist(rng)); // Выбираем случайного специалиста
    const char *specName = (p->specialist_type == DENTIST) ? "стоматологу"
                           : (p->specialist_type == SURGEON)
                               ? "хирургу"
                               : "терапевту"; // Определяем имя специалиста
    log_event("Дежурный D%d направил пациента P%d к %s\n", did, p->id,
              specName); // Логируем направление к специалисту

    // Добавляем пациента в очередь к специалисту
    pthread_mutex_lock(
        &specialistLock[p->specialist_type]); // Захватываем мьютекс очереди
                                              // выбранного специалиста
    specialistQueue[p->specialist_type].push(
        p); // Добавляем пациента в очередь специалиста
    pthread_cond_signal(
        &specialistNotEmpty[p->specialist_type]); // Сигнализируем, что очередь
                                                  // у специалиста не пуста
    pthread_mutex_unlock(
        &specialistLock[p->specialist_type]); // Освобождаем мьютекс очереди
                                              // специалиста

    // Увеличиваем счетчик направленных пациентов
    pthread_mutex_lock(
        &patientsToSpecialistLock); // Захватываем мьютекс счетчика
    patientsToSpecialist++; // Инкрементируем счетчик
    bool now_all_sent =
        (patientsToSpecialist ==
         N); // Проверяем, достигли ли мы общего количества пациентов
    pthread_mutex_unlock(
        &patientsToSpecialistLock); // Освобождаем мьютекс счетчика

    // Если теперь все пациенты направлены, разбудим все потоки, чтобы они
    // проверили свои условия
    if (now_all_sent) { // Если все пациенты уже в очередях к специалистам
      pthread_mutex_lock(
          &commonQueueLock); // Захватываем мьютекс очереди дежурных
      pthread_cond_broadcast(
          &commonQueueNotEmpty); // Будим всех дежурных врачей
      pthread_mutex_unlock(
          &commonQueueLock); // Освобождаем мьютекс очереди дежурных
      for (int i = 0; i < 3; i++) { // Для всех специалистов
        pthread_mutex_lock(
            &specialistLock[i]); // Захватываем мьютекс очереди специалиста
        pthread_cond_broadcast(&specialistNotEmpty[i]); // Будим специалистов
        pthread_mutex_unlock(&specialistLock[i]); // Освобождаем мьютекс
      }
    }
  }

end_duty:
  log_event("Дежурный D%d закончил свой рабочий день\n",
            did); // Логируем завершение дежурного врача
  return NULL; // Завершаем поток дежурного врача
}

// Поток специалиста
void *specialist_thread(void *arg) {
  int sid = *(int *)arg; // Извлекаем id специалиста
  delete (int *)arg;     // Освобождаем память под id

  const char *specName = (sid == 0) ? "Стоматолог"
                         : (sid == 1)
                             ? "Хирург"
                             : "Терапевт"; // Определяем имя специалиста по id

  while (true) { // Бесконечный цикл (до выхода)
    pthread_mutex_lock(
        &specialistLock[sid]); // Захватываем мьютекс очереди этого специалиста
    while (specialistQueue[sid].empty()) { // Пока очередь специалиста пуста
      pthread_mutex_lock(
          &patientsToSpecialistLock); // Захватываем мьютекс счетчика
      bool all_sent =
          (patientsToSpecialist == N); // Проверяем, все ли пациенты направлены
      pthread_mutex_unlock(
          &patientsToSpecialistLock); // Освобождаем мьютекс счетчика

      if (all_sent &&
          specialistQueue[sid]
              .empty()) { // Если все пациенты направлены и очередь пуста
        pthread_mutex_unlock(
            &specialistLock[sid]); // Освобождаем мьютекс очереди специалиста
        goto end_specialist; // Завершаем работу этого специалиста
      }

      pthread_cond_wait(
          &specialistNotEmpty[sid],
          &specialistLock[sid]); // Ждем появления пациента в очереди
    }

    Patient *p = specialistQueue[sid].front(); // Берем пациента из очереди
    specialistQueue[sid].pop(); // Удаляем его из очереди
    pthread_mutex_unlock(
        &specialistLock[sid]); // Освобождаем мьютекс очереди специалиста

    // Лечение пациента
    log_event("%s начал лечение пациента P%d\n", specName,
              p->id); // Логируем начало лечения
    sleep(t_s);       // Имитируем время лечения
    log_event("%s закончил лечение пациента P%d\n", specName,
              p->id); // Логируем окончание лечения

    // Уведомляем пациента
    pthread_mutex_lock(&p->patientLock); // Захватываем мьютекс пациента
    pthread_cond_signal(&p->treated); // Сигнализируем, что пациент вылечен
    pthread_mutex_unlock(&p->patientLock); // Освобождаем мьютекс пациента
  }

end_specialist:
  log_event("%s закончил свой рабочий день\n",
            specName); // Логируем завершение специалиста
  return NULL;         // Завершаем поток специалиста
}

// Функция отображения справки
void print_help() {
  std::cout
      << "Usage: program [options]\n" // Выводим подсказку по использованию
      << "Options:\n"
      << "  -f <file>      Load parameters from configuration file\n"
      << "  -n <number>    Number of patients\n"
      << "  -t_d <ms>      Time for duty doctor to process a patient\n"
      << "  -t_s <ms>      Time for specialist to treat a patient\n"
      << "  -o <file>      Output log file\n"
      << "  --help [-h]    Display this help message\n";
}

// Функция отображения заданных для симуляции параметров
void log_parameters() {
  log_event("Параметры задачи:\n"); // Логируем параметры задачи
  log_event("Количество пациентов: %d\n", N); // Логируем количество пациентов
  log_event("Время приема дежурного врача (мс): %d\n",
            t_d); // Логируем время приема дежурного врача
  log_event("Время приема специалиста (мс): %d\n",
            t_s); // Логируем время приема специалиста
  log_event("Файл для вывода логов: %s\n\n",
            output_filename.c_str()); // Логируем имя файла для логов
}

// Функция парсинга командной строки или файла
bool parse_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++) { // Идем по всем аргументам командной строки
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_help(); // Если запрошена помощь, выводим ее
      exit(0);      // И выходим
    } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
      from_file = true; // Отмечаем что нужно читать из файла
      config_filename = argv[++i]; // Запоминаем имя файла
    } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      N = atoi(argv[++i]); // Читаем количество пациентов
    } else if (strcmp(argv[i], "-t_d") == 0 && i + 1 < argc) {
      t_d = atoi(argv[++i]); // Читаем время дежурного врача
    } else if (strcmp(argv[i], "-t_s") == 0 && i + 1 < argc) {
      t_s = atoi(argv[++i]); // Читаем время специалиста
    } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      output_filename = argv[++i]; // Читаем имя файла для логов
    }
  }

  if (from_file) { // Если нужно читать из файла конфигурации
    std::ifstream fin(config_filename.c_str()); // Открываем файл
    if (!fin) { // Если не удалось открыть
      std::cerr
          << "Не удалось открыть файл конфигурации\n"; // Сообщаем об ошибке
      return false; // Возвращаем false
    }
    std::string line;                 // Строка для чтения
    while (std::getline(fin, line)) { // Читаем построчно
      if (line.find("n=") == 0) {
        N = atoi(line.substr(2).c_str()); // Читаем N
      } else if (line.find("t_d=") == 0) {
        t_d = atoi(line.substr(4).c_str()); // Читаем t_d
      } else if (line.find("t_s=") == 0) {
        t_s = atoi(line.substr(4).c_str()); // Читаем t_s
      } else if (line.find("o=") == 0) {
        output_filename = line.substr(7); // Читаем имя файла логов
      }
    }
  }

  return true; // Возвращаем true если всё ОК
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "ru"); // Устанавливаем локаль (русский язык)
  srand(time(NULL)); // Инициализируем генератор случайных чисел

  if (!parse_args(argc, argv)) { // Парсим аргументы
    std::cerr << "Ошибка при чтении параметров\n"; // Если ошибка при чтении
    return 1; // Выходим с кодом ошибки
  }

  log_file =
      fopen(output_filename.c_str(), "w+"); // Открываем файл логов на запись
  if (!log_file) { // Если не удалось открыть
    std::cerr << "Не удалось открыть файл вывода\n"; // Сообщаем об ошибке
    return 1; // Выходим с кодом ошибки
  }

  log_parameters(); // Логируем параметры задачи

  // Инициализируем мьютексы для специалистов
  for (int i = 0; i < 3; i++) {
    pthread_mutex_init(&specialistLock[i],
                       NULL); // Инициализируем мьютекс специалиста
    pthread_cond_init(&specialistNotEmpty[i],
                      NULL); // Инициализируем условную переменную специалиста
  }

  // Создаем потоки дежурных врачей
  for (int i = 0; i < 2; i++) {
    int *id = new int(i + 1); // Выделяем память под id врача
    pthread_create(&duty_docs[i], NULL, duty_doctor_thread,
                   (void *)id); // Создаем поток дежурного врача
  }

  // Создаем потоки специалистов
  for (int i = 0; i < 3; i++) {
    int *id = new int(i); // Выделяем память под id специалиста
    pthread_create(&specialists[i], NULL, specialist_thread,
                   (void *)id); // Создаем поток специалиста
  }

  // Создаем потоки пациентов
  patients = new pthread_t[N]; // Выделяем память под массив потоков пациентов
  for (int i = 0; i < N; i++) {
    int *pid = new int(i + 1); // Выделяем память под id пациента
    pthread_create(&patients[i], NULL, patient_thread,
                   (void *)pid); // Создаем поток пациента
  }

  // Ждем завершения всех потоков пациентов
  for (int i = 0; i < N; i++) {
    pthread_join(patients[i], NULL); // Ждем завершения потока пациента
  }

  log_event("Все пациенты вылечены\n"); // Логируем, что все пациенты вылечены

  for (int i = 0; i < 2; i++) {
    pthread_mutex_lock(
        &commonQueueLock); // Захватываем мьютекс очереди дежурных
    pthread_cond_broadcast(&commonQueueNotEmpty); // Будим всех дежурных (может
                                                  // быть лишним, но оставим)
    pthread_mutex_unlock(&commonQueueLock); // Освобождаем мьютекс
  }

  // Все пациенты уже пришли и ушли. Дежурные закончат, когда направят всех N
  // пациентов. После чего завершим дежурных.
  for (int i = 0; i < 2; i++) {
    pthread_join(duty_docs[i], NULL); // Ждем завершения потоков дежурных врачей
  }

  // Это должно было уже выполниться при завершении всех потоков пациентов,
  // но... на всякий случай тут тоже убедимся.
  pthread_mutex_lock(&commonQueueLock); // Захватываем мьютекс очереди дежурных
  pthread_cond_broadcast(
      &commonQueueNotEmpty); // Будим дежурных еще раз (на всякий случай)
  pthread_mutex_unlock(&commonQueueLock); // Освобождаем мьютекс

  // Все дежурные завершились, значит patients_to_specialist == N.
  // Пробудим всех специалистов, если кто-то ещё спит.
  for (int i = 0; i < 3; i++) {
    pthread_mutex_lock(&specialistLock[i]); // Захватываем мьютекс специалиста
    pthread_cond_broadcast(&specialistNotEmpty[i]); // Будим специалистов
    pthread_mutex_unlock(&specialistLock[i]); // Освобождаем мьютекс
  }

  // Ждем завершения всех специалистов
  for (int i = 0; i < 3; i++) {
    pthread_join(specialists[i], NULL); // Ждем завершения потоков специалистов
  }

  log_event(
      "Рабочий день в больнице завершен\n"); // Логируем завершение рабочего дня

  // Удаляем мьютексы и условные переменные
  for (int i = 0; i < 3; i++) {
    pthread_mutex_destroy(&specialistLock[i]); // Уничтожаем мьютекс специалиста
    pthread_cond_destroy(
        &specialistNotEmpty[i]); // Уничтожаем условную переменную специалиста
  }
  pthread_mutex_destroy(
      &commonQueueLock); // Уничтожаем мьютекс очереди дежурных
  pthread_cond_destroy(
      &commonQueueNotEmpty); // Уничтожаем условную переменную для дежурных

  pthread_mutex_destroy(&consoleLogLock); // Уничтожаем мьютекс консоли
  pthread_mutex_destroy(&fileLogLock); // Уничтожаем мьютекс файла
  pthread_mutex_destroy(
      &patientsToSpecialistLock); // Уничтожаем мьютекс счетчика

  fclose(log_file);  // Закрываем файл логов
  delete[] patients; // Освобождаем память под массив потоков пациентов

  return 0; // Возвращаем 0 - успешное завершение программы
}
