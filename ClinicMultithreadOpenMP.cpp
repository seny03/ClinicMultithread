#include <chrono> // Для работы с временем
#include <cstdio> // Для функций ввода-вывода C (printf, fprintf)
#include <cstdlib> // Для функций стандартной библиотеки C (atoi, rand и др.)
#include <cstring> // Для функций работы со строками C (strcmp)
#include <fstream> // Для работы с файлами (ifstream, ofstream)
#include <iostream> // Для стандартного ввода-вывода C++ (cin, cout)
#include <omp.h> // Для OpenMP
#include <queue> // Для контейнера очередь (std::queue)
#include <random> // Для генераторов случайных чисел (std::mt19937)
#include <stdarg.h> // Для работы с variadic аргументами (va_list)
#include <string> // Для класса std::string

#if _WIN32
#include <windows.h> // Для функции Sleep на Windows
#define sleep_ms(x) Sleep(x) // Определяем sleep_ms как Sleep (миллисекунды)
#else
#include <unistd.h> // Для функции usleep на UNIX/Linux
#define sleep_ms(x)                                                            \
  usleep(1000L * (x)) // Определяем sleep_ms через usleep (миллисекунды)
#endif

// Перечисление типов специалистов
enum SpecialistType { NONE = -1, DENTIST = 0, SURGEON = 1, THERAPIST = 2 };

// Структура пациента
struct Patient {
  int id; // Идентификатор пациента
  SpecialistType specialist_type; // Тип специалиста, к которому направлен
  bool treated; // Флаг, указывающий, вылечен ли пациент
};

// Глобальные переменные
int N = 5;      // Число пациентов
int t_d = 1000; // Время приема дежурного врача (мс)
int t_s = 2000; // Время приема специалиста (мс)
std::string output_filename = "clinic_log.txt"; // Имя файла для вывода логов
bool from_file = false; // Флаг чтения параметров из файла
std::string config_filename; // Имя файла конфигурации

// Очередь к дежурным
std::queue<Patient *> commonQueue; // Очередь пациентов к дежурным врачам
omp_lock_t commonQueueLock; // Блокировка для очереди дежурных

// Очереди к специалистам: стоматолог(0), хирург(1), терапевт(2)
std::queue<Patient *> specialistQueue[3]; // Три очереди для трех специалистов
omp_lock_t specialistLock[3]; // Блокировки для каждой очереди специалистов

// Spinlocks для логирования и счетчика
omp_lock_t consoleLogLock; // Блокировка для логирования в консоль
omp_lock_t fileLogLock; // Блокировка для логирования в файл
omp_lock_t patientsToSpecialistLock; // Блокировка для доступа к счетчику

// Счетчик направленных пациентов
int patientsToSpecialist = 0;

// Файл вывода
FILE *log_file = NULL; // Указатель на файл логов
auto program_start =
    std::chrono::high_resolution_clock::now(); // Время старта программы

// Генератор случайных чисел
std::mt19937
    rng(42); // Стандартный генератор случайных чисел с фиксированным сидом
std::uniform_int_distribution<int>
    specialist_dist(0, 2); // Распределение для выбора специалиста

// Функция для получения времени с момента старта программы
std::string get_time_since_start() {
  auto now = std::chrono::high_resolution_clock::now(); // Текущее время
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - program_start)
          .count();                    // Прошедшее время в мс
  int minutes = elapsed / 60000;       // Переводим в минуты
  int seconds = (elapsed / 1000) % 60; // Остаток в секундах
  int milliseconds = elapsed % 1000;   // Миллисекунды
  char buffer[30];
  sprintf(buffer, "[%02d:%02d:%03d]", minutes, seconds,
          milliseconds); // Форматируем строку времени
  return std::string(buffer);
}

// Функция логирования
void log_event(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  va_list args2;
  va_copy(args2, args);

  std::string time_str = get_time_since_start();

  // Вывод в консоль с использованием блокировки
  omp_set_lock(&consoleLogLock);
  printf("%s ", time_str.c_str());
  vprintf(fmt, args);
  omp_unset_lock(&consoleLogLock);

  // Вывод в файл с использованием блокировки
  if (log_file) {
    omp_set_lock(&fileLogLock);
    fprintf(log_file, "%s ", time_str.c_str());
    vfprintf(log_file, fmt, args2);
    omp_unset_lock(&fileLogLock);
    va_end(args2);
  }
  va_end(args);
}

// Функция создания пациента и добавления в очередь к дежурным
void create_patient(int pid) {
  Patient *p = new Patient();
  p->id = pid;
  p->specialist_type = NONE;
  p->treated = false;

  // Добавляем пациента в очередь к дежурным
  omp_set_lock(&commonQueueLock);
  commonQueue.push(p);
  log_event("Patient P%d entered the queue to the duty doctors\n", p->id);
  omp_unset_lock(&commonQueueLock);
}

// Функция обработки пациента дежурным врачом
void duty_doctor(int did) {
  while (true) {
    Patient *p = nullptr;

    // Попытка взять пациента из очереди
    omp_set_lock(&commonQueueLock);
    if (!commonQueue.empty()) {
      p = commonQueue.front();
      commonQueue.pop();
    }
    omp_unset_lock(&commonQueueLock);

    if (p != nullptr) {
      // Принимаем пациента
      log_event("Duty Doctor D%d accepted patient P%d\n", did, p->id);
      sleep_ms(t_d); // Имитируем время приема

      // Определяем специалиста
      p->specialist_type = static_cast<SpecialistType>(specialist_dist(rng));
      const char *specName = (p->specialist_type == DENTIST)   ? "Dentist"
                             : (p->specialist_type == SURGEON) ? "Surgeon"
                                                               : "Therapist";
      log_event("Duty Doctor D%d referred patient P%d to %s\n", did, p->id,
                specName);

      // Добавляем пациента в очередь к специалисту
      omp_set_lock(&specialistLock[p->specialist_type]);
      specialistQueue[p->specialist_type].push(p);
      omp_unset_lock(&specialistLock[p->specialist_type]);

      // Увеличиваем счетчик направленных пациентов
      omp_set_lock(&patientsToSpecialistLock);
      patientsToSpecialist++;
      bool now_all_sent = (patientsToSpecialist == N);
      omp_unset_lock(&patientsToSpecialistLock);

      // Если все пациенты направлены, завершаем работу
      if (now_all_sent) {
        break;
      }
    } else {
      // Если очередь пуста, проверяем, все ли пациенты направлены
      omp_set_lock(&patientsToSpecialistLock);
      bool all_sent = (patientsToSpecialist == N);
      omp_unset_lock(&patientsToSpecialistLock);

      if (all_sent) {
        break; // Завершаем работу врача
      } else {
        // Ждем немного перед повторной проверкой
        sleep_ms(100); // 100 мс
      }
    }
  }

  log_event("Duty Doctor D%d ended his workday\n", did);
}

// Функция обработки пациента специалистом
void specialist(int sid) {
  const char *specName = (sid == DENTIST)   ? "Dentist"
                         : (sid == SURGEON) ? "Surgeon"
                                            : "Therapist";

  while (true) {
    Patient *p = nullptr;

    // Попытка взять пациента из очереди специалиста
    omp_set_lock(&specialistLock[sid]);
    if (!specialistQueue[sid].empty()) {
      p = specialistQueue[sid].front();
      specialistQueue[sid].pop();
    }
    omp_unset_lock(&specialistLock[sid]);

    if (p != nullptr) {
      // Лечение пациента
      log_event("%s started treating patient P%d\n", specName, p->id);
      sleep_ms(t_s); // Имитируем время лечения
      log_event("%s finished treating patient P%d\n", specName, p->id);

      // Уведомляем пациента
      p->treated = true;
    } else {
      // Если очередь пуста, проверяем, все ли пациенты направлены
      omp_set_lock(&patientsToSpecialistLock);
      bool all_sent = (patientsToSpecialist == N);
      omp_unset_lock(&patientsToSpecialistLock);

      if (all_sent) {
        break; // Завершаем работу специалиста
      } else {
        // Ждем немного перед повторной проверкой
        sleep_ms(100); // 100 мс
      }
    }
  }

  log_event("%s ended his workday\n", specName);
}

// Функция отображения справки
void print_help() {
  std::cout << "Usage: program [options]\n"
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
  log_event("Simulation Parameters:\n");
  log_event("Number of patients: %d\n", N);
  log_event("Duty doctor's processing time (ms): %d\n", t_d);
  log_event("Specialist's treatment time (ms): %d\n", t_s);
  log_event("Log file: %s\n\n", output_filename.c_str());
}

// Функция парсинга командной строки или файла
bool parse_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++) { // Идем по всем аргументам командной строки
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_help(); // Если запрошена помощь, выводим ее
      exit(0);      // И выходим
    } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
      from_file = true;
      config_filename = argv[++i];
    } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
      N = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-t_d") == 0 && i + 1 < argc) {
      t_d = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-t_s") == 0 && i + 1 < argc) {
      t_s = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      output_filename = argv[++i];
    }
  }

  if (from_file) { // Если нужно читать из файла конфигурации
    std::ifstream fin(config_filename.c_str());
    if (!fin) {
      std::cerr << "Failed to open configuration file\n";
      return false;
    }
    std::string line;
    while (std::getline(fin, line)) { // Читаем построчно
      if (line.find("n=") == 0) {
        N = atoi(line.substr(2).c_str());
      } else if (line.find("t_d=") == 0) {
        t_d = atoi(line.substr(4).c_str());
      } else if (line.find("t_s=") == 0) {
        t_s = atoi(line.substr(4).c_str());
      } else if (line.find("o=") == 0) {
        output_filename = line.substr(2);
      }
    }
  }

  return true;
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "ru"); // Устанавливаем локаль (русский язык)

  // Инициализируем блокировки
  omp_init_lock(&commonQueueLock);
  for (int i = 0; i < 3; i++) {
    omp_init_lock(&specialistLock[i]);
  }
  omp_init_lock(&consoleLogLock);
  omp_init_lock(&fileLogLock);
  omp_init_lock(&patientsToSpecialistLock);

  // Парсим аргументы командной строки или файла конфигурации
  if (!parse_args(argc, argv)) {
    std::cerr << "Error reading parameters\n";
    return 1;
  }

  // Открываем файл логов на запись
  log_file = fopen(output_filename.c_str(), "w+");
  if (!log_file) {
    std::cerr << "Failed to open output file\n";
    return 1;
  }

  log_parameters(); // Логируем параметры задачи

// Запускаем параллельный регион
#pragma omp parallel
  {
#pragma omp single
    {
      // Создаем потоки дежурных врачей
      for (int i = 0; i < 2; i++) {
        int did = i + 1;
#pragma omp task firstprivate(did)
        { duty_doctor(did); }
      }

      // Создаем потоки специалистов
      for (int i = 0; i < 3; i++) {
        int sid = i;
#pragma omp task firstprivate(sid)
        { specialist(sid); }
      }

      // Создаем потоки пациентов
      for (int i = 0; i < N; i++) {
        int pid = i + 1;
#pragma omp task firstprivate(pid)
        { create_patient(pid); }
      }
    }
  } // Конец параллельного региона

  log_event(
      "The hospital workday has ended\n"); // Логируем завершение рабочего дня

  // Уничтожаем блокировки
  omp_destroy_lock(&commonQueueLock);
  for (int i = 0; i < 3; i++) {
    omp_destroy_lock(&specialistLock[i]);
  }
  omp_destroy_lock(&consoleLogLock);
  omp_destroy_lock(&fileLogLock);
  omp_destroy_lock(&patientsToSpecialistLock);

  fclose(log_file); // Закрываем файл логов

  return 0; // Успешное завершение программы
}
