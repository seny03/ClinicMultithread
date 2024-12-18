#include <chrono> // Для работы с временем
#include <cstdio> // Для функций ввода-вывода C (printf, fprintf)
#include <cstdlib> // Для функций стандартной библиотеки C (atoi, rand и др.)
#include <cstring> // Для функций работы со строками C (strcmp)
#include <fstream> // Для работы с файлами (ifstream, ofstream)
#include <iostream> // Для стандартного ввода-вывода C++ (cin, cout)
#include <pthread.h> // Для работы с потоками POSIX (pthread_*)
#include <queue>     // Для контейнера очередь (std::queue)
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
  pthread_cond_t treated =
      PTHREAD_COND_INITIALIZER; // Условная переменная для ожидания лечения
  pthread_mutex_t patientLock; // Мьютекс для синхронизации состояния пациента
};

// Глобальные переменные
int N = 5;      // Число пациентов
int t_d = 1000; // Время приема дежурного врача (мс)
int t_s = 2000; // Время приема специалиста (мс)
std::string output_filename = "clinic_log.txt"; // Имя файла для вывода логов
bool from_file = false; // Флаг чтения параметров из файла
std::string config_filename; // Имя файла конфигурации

// Потоки
pthread_t *patients; // Массив потоков пациентов
pthread_t duty_docs[2]; // Массив потоков дежурных врачей (2 врача)
pthread_t specialists[3]; // Массив потоков специалистов (3 специалиста)

// Очередь к дежурным
std::queue<Patient *> commonQueue; // Очередь пациентов к дежурным врачам
pthread_mutex_t commonQueueLock; // Мьютекс для очереди дежурных
pthread_cond_t commonQueueNotEmpty =
    PTHREAD_COND_INITIALIZER; // Условная переменная для оповещения о новых
                              // пациентах

// Очереди к специалистам: стоматолог(0), хирург(1), терапевт(2)
std::queue<Patient *> specialistQueue[3]; // Три очереди для трех специалистов
pthread_mutex_t specialistLock[3]; // Мьютексы для каждой очереди специалистов
pthread_cond_t
    specialistNotEmpty[3]; // Условные переменные для очередей специалистов

// Spinlocks для логирования и счетчика
pthread_spinlock_t consoleLogLock; // Spinlock для логирования в консоль
pthread_spinlock_t fileLogLock; // Spinlock для логирования в файл
pthread_spinlock_t patientsToSpecialistLock; // Spinlock для доступа к счетчику

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

// Глобальный атрибут для адаптивных мьютексов
pthread_mutexattr_t adaptive_attr;

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

  // Вывод в консоль с использованием spinlock
  pthread_spin_lock(&consoleLogLock);
  printf("%s ", time_str.c_str());
  vprintf(fmt, args);
  pthread_spin_unlock(&consoleLogLock);

  // Вывод в файл с использованием spinlock
  if (log_file) {
    pthread_spin_lock(&fileLogLock);
    fprintf(log_file, "%s ", time_str.c_str());
    vfprintf(log_file, fmt, args2);
    pthread_spin_unlock(&fileLogLock);
    va_end(args2);
  }
  va_end(args);
}

// Поток пациента
void *patient_thread(void *arg) {
  int pid = *(int *)arg; // Извлекаем id пациента из аргумента
  delete (int *)arg; // Освобождаем память под id

  Patient *p = new Patient(); // Создаем новый объект пациента
  p->id = pid;
  p->specialist_type = NONE;

  // Инициализируем адаптивный мьютекс пациента
  pthread_mutex_init(&p->patientLock, &adaptive_attr);

  // Добавляем пациента в очередь к дежурным
  pthread_mutex_lock(&commonQueueLock);
  commonQueue.push(p);
  log_event("Пациент P%d встал в очередь к дежурным\n", p->id);
  pthread_cond_signal(&commonQueueNotEmpty);
  pthread_mutex_unlock(&commonQueueLock);

  // Ждем, пока пациент будет вылечен
  pthread_mutex_lock(&p->patientLock);
  pthread_cond_wait(&p->treated, &p->patientLock);
  pthread_mutex_unlock(&p->patientLock);

  log_event("Пациент P%d полностью вылечен и пошел домой\n", p->id);
  delete p; // Освобождаем память под пациента
  return NULL;
}

// Поток дежурного врача
void *duty_doctor_thread(void *arg) {
  int did = *(int *)arg; // Извлекаем id дежурного врача
  delete (int *)arg;     // Освобождаем память под id

  while (true) {
    pthread_mutex_lock(&commonQueueLock);

    while (commonQueue.empty()) {
      // Проверяем, не обработаны ли все пациенты
      pthread_spin_lock(&patientsToSpecialistLock);
      bool all_sent = (patientsToSpecialist == N);
      pthread_spin_unlock(&patientsToSpecialistLock);

      if (all_sent) {
        pthread_mutex_unlock(&commonQueueLock);
        goto end_duty; // Завершаем работу врача
      }

      pthread_cond_wait(&commonQueueNotEmpty, &commonQueueLock);
    }

    // Забираем пациента из очереди
    Patient *p = commonQueue.front();
    commonQueue.pop();
    pthread_mutex_unlock(&commonQueueLock);

    // Принимаем пациента
    log_event("Дежурный D%d принял пациента P%d\n", did, p->id);
    sleep_ms(t_d); // Имитируем время приема

    // Определяем специалиста
    p->specialist_type = static_cast<SpecialistType>(specialist_dist(rng));
    const char *specName = (p->specialist_type == DENTIST) ? "стоматологу"
                           : (p->specialist_type == SURGEON) ? "хирургу"
                                                             : "терапевту";
    log_event("Дежурный D%d направил пациента P%d к %s\n", did, p->id,
              specName);

    // Добавляем пациента в очередь к специалисту
    pthread_mutex_lock(&specialistLock[p->specialist_type]);
    specialistQueue[p->specialist_type].push(p);
    pthread_cond_signal(&specialistNotEmpty[p->specialist_type]);
    pthread_mutex_unlock(&specialistLock[p->specialist_type]);

    // Увеличиваем счетчик направленных пациентов
    pthread_spin_lock(&patientsToSpecialistLock);
    patientsToSpecialist++;
    bool now_all_sent = (patientsToSpecialist == N);
    pthread_spin_unlock(&patientsToSpecialistLock);

    // Если все пациенты направлены, разбудим всех дежурных и специалистов
    if (now_all_sent) {
      pthread_mutex_lock(&commonQueueLock);
      pthread_cond_broadcast(&commonQueueNotEmpty);
      pthread_mutex_unlock(&commonQueueLock);

      for (int i = 0; i < 3; i++) {
        pthread_mutex_lock(&specialistLock[i]);
        pthread_cond_broadcast(&specialistNotEmpty[i]);
        pthread_mutex_unlock(&specialistLock[i]);
      }
    }
  }

end_duty:
  log_event("Дежурный D%d закончил свой рабочий день\n", did);
  return NULL;
}

// Поток специалиста
void *specialist_thread(void *arg) {
  int sid = *(int *)arg; // Извлекаем id специалиста
  delete (int *)arg;     // Освобождаем память под id

  const char *specName = (sid == DENTIST)   ? "Стоматолог"
                         : (sid == SURGEON) ? "Хирург"
                                            : "Терапевт";

  while (true) {
    pthread_mutex_lock(&specialistLock[sid]);

    while (specialistQueue[sid].empty()) {
      // Проверяем, все ли пациенты направлены
      pthread_spin_lock(&patientsToSpecialistLock);
      bool all_sent = (patientsToSpecialist == N);
      pthread_spin_unlock(&patientsToSpecialistLock);

      if (all_sent && specialistQueue[sid].empty()) {
        pthread_mutex_unlock(&specialistLock[sid]);
        goto end_specialist; // Завершаем работу специалиста
      }

      pthread_cond_wait(&specialistNotEmpty[sid], &specialistLock[sid]);
    }

    // Забираем пациента из очереди
    Patient *p = specialistQueue[sid].front();
    specialistQueue[sid].pop();
    pthread_mutex_unlock(&specialistLock[sid]);

    // Лечение пациента
    log_event("%s начал лечение пациента P%d\n", specName, p->id);
    sleep_ms(t_s); // Имитируем время лечения
    log_event("%s закончил лечение пациента P%d\n", specName, p->id);

    // Уведомляем пациента
    pthread_mutex_lock(&p->patientLock);
    pthread_cond_signal(&p->treated);
    pthread_mutex_unlock(&p->patientLock);
  }

end_specialist:
  log_event("%s закончил свой рабочий день\n", specName);
  return NULL;
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
  log_event("Параметры задачи:\n");
  log_event("Количество пациентов: %d\n", N);
  log_event("Время приема дежурного врача (мс): %d\n", t_d);
  log_event("Время приема специалиста (мс): %d\n", t_s);
  log_event("Файл для вывода логов: %s\n\n", output_filename.c_str());
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
      std::cerr << "Не удалось открыть файл конфигурации\n";
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
        output_filename =
            line.substr(2); // Исправлено: substr(2) вместо substr(7)
      }
    }
  }

  return true;
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "ru"); // Устанавливаем локаль (русский язык)
  srand(time(NULL)); // Инициализируем генератор случайных чисел

  // Инициализируем атрибут для адаптивных мьютексов
  pthread_mutexattr_init(&adaptive_attr);
  pthread_mutexattr_settype(&adaptive_attr, PTHREAD_MUTEX_ADAPTIVE_NP);

  // Инициализируем мьютексы для специалистов с адаптивным типом
  for (int i = 0; i < 3; i++) {
    pthread_mutex_init(
        &specialistLock[i],
        &adaptive_attr); // Инициализируем мьютекс специалиста как адаптивный
    pthread_cond_init(&specialistNotEmpty[i],
                      NULL); // Инициализируем условную переменную специалиста
  }

  // Инициализируем мьютекс очереди дежурных как адаптивный
  pthread_mutex_init(&commonQueueLock, &adaptive_attr);

  // Инициализируем spinlock'и
  pthread_spin_init(&consoleLogLock, 0); // Инициализируем spinlock консоли
  pthread_spin_init(&fileLogLock, 0); // Инициализируем spinlock файла
  pthread_spin_init(&patientsToSpecialistLock,
                    0); // Инициализируем spinlock счетчика

  // Парсим аргументы командной строки или файла конфигурации
  if (!parse_args(argc, argv)) {
    std::cerr << "Ошибка при чтении параметров\n";
    return 1;
  }

  // Открываем файл логов на запись
  log_file = fopen(output_filename.c_str(), "w+");
  if (!log_file) {
    std::cerr << "Не удалось открыть файл вывода\n";
    return 1;
  }

  log_parameters(); // Логируем параметры задачи

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

  log_event(
      "Все пациенты вылечены\n"); // Логируем завершение лечения всех пациентов

  // Разбудим дежурных врачей, чтобы они могли завершить работу
  for (int i = 0; i < 2; i++) {
    pthread_mutex_lock(&commonQueueLock);
    pthread_cond_broadcast(&commonQueueNotEmpty); // Будим всех дежурных врачей
    pthread_mutex_unlock(&commonQueueLock);
  }

  // Ждем завершения всех потоков дежурных врачей
  for (int i = 0; i < 2; i++) {
    pthread_join(duty_docs[i], NULL);
  }

  // Разбудим специалистов, чтобы они могли завершить работу, если все пациенты
  // направлены
  for (int i = 0; i < 3; i++) {
    pthread_mutex_lock(&specialistLock[i]);
    pthread_cond_broadcast(&specialistNotEmpty[i]); // Будим специалистов
    pthread_mutex_unlock(&specialistLock[i]);
  }

  // Ждем завершения всех потоков специалистов
  for (int i = 0; i < 3; i++) {
    pthread_join(specialists[i], NULL);
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
      &commonQueueNotEmpty); // Уничтожаем условную переменную очереди дежурных

  // Разрушение spinlock'ов
  pthread_spin_destroy(&consoleLogLock); // Уничтожаем spinlock консоли
  pthread_spin_destroy(&fileLogLock); // Уничтожаем spinlock файла
  pthread_spin_destroy(
      &patientsToSpecialistLock); // Уничтожаем spinlock счетчика

  // Разрушение адаптивных атрибутов
  pthread_mutexattr_destroy(&adaptive_attr);

  fclose(log_file);  // Закрываем файл логов
  delete[] patients; // Освобождаем память под массив потоков пациентов

  return 0; // Успешное завершение программы
}
