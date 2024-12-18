#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <queue>
#include <random>
#include <string>

#if _WIN32
#include <windows.h>
#define sleep(x) Sleep(x)
#else
#include <stdarg.h>
#include <unistd.h>
#define sleep(x) usleep(1000L * x)
#endif

// Структура пациента
enum SpecialistType { NONE = -1, DENTIST = 0, SURGEON = 1, THERAPIST = 2 };

struct Patient {
  int id;
  SpecialistType specialist_type;
  pthread_cond_t treated =
      PTHREAD_COND_INITIALIZER; // Условная переменная для ожидания лечения
  pthread_mutex_t patientLock =
      PTHREAD_MUTEX_INITIALIZER; // Мьютекс для синхронизации пациента
};

// Глобальные переменные (для простоты)
int N = 5;      // число пациентов
int t_d = 1000; // время приема дежурного
int t_s = 2000; // время приема специалиста
std::string output_filename = "data/clinic_log.txt";
bool from_file = false;
std::string config_filename;

// Потоки
pthread_t *patients;
pthread_t duty_docs[2];
pthread_t specialists[3];

// Очередь к дежурным
std::queue<Patient *> commonQueue;
pthread_mutex_t commonQueueLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t commonQueueNotEmpty = PTHREAD_COND_INITIALIZER;

// Очереди к специалистам: стоматолог(0), хирург(1), терапевт(2)
std::queue<Patient *> specialistQueue[3];
pthread_mutex_t specialistLock[3];
pthread_cond_t specialistNotEmpty[3];

pthread_mutex_t consoleLogLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t fileLogLock = PTHREAD_MUTEX_INITIALIZER;

// Новые глобальные переменные для подсчета направленных к специалисту пациентов
int patients_to_specialist = 0;
pthread_mutex_t patients_to_specialist_lock = PTHREAD_MUTEX_INITIALIZER;

// Файл вывода
FILE *log_file = NULL;
auto program_start = std::chrono::high_resolution_clock::now();

// Генератор случайных чисел
std::mt19937 rng(42);
std::uniform_int_distribution<int> specialist_dist(0, 2);

// Функция для получения времени с момента старта программы
std::string get_time_since_start() {
  auto now = std::chrono::high_resolution_clock::now();
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - program_start)
          .count();
  int minutes = elapsed / 60000;
  int seconds = (elapsed / 1000) % 60;
  int milliseconds = elapsed % 1000;
  char buffer[30];
  sprintf(buffer, "[%02d:%02d:%03d]", minutes, seconds, milliseconds);
  return std::string(buffer);
}

// Функция логирования
void log_event(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  va_list args2;
  va_copy(args2, args);

  std::string time_str = get_time_since_start();
  // Вывод в консоль
  pthread_mutex_lock(&consoleLogLock);
  printf("%s ", time_str.c_str());
  vprintf(fmt, args);
  pthread_mutex_unlock(&consoleLogLock);

  // Вывод в файл
  if (log_file) {
    pthread_mutex_lock(&fileLogLock);
    fprintf(log_file, "%s ", time_str.c_str());
    vfprintf(log_file, fmt, args2);
    pthread_mutex_unlock(&fileLogLock);
    va_end(args2);
  }
  va_end(args);
}

// Поток пациента
void *patient_thread(void *arg) {
  int pid = *(int *)arg;
  delete (int *)arg; // освободим память под id

  Patient *p = new Patient();
  p->id = pid;
  p->specialist_type = NONE;

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
  delete p;
  return NULL;
}

// Поток дежурного врача
void *duty_doctor_thread(void *arg) {
  int did = *(int *)arg;
  delete (int *)arg;

  while (true) {
    pthread_mutex_lock(&commonQueueLock);

    while (commonQueue.empty()) {
      // Проверяем, не обработаны ли все пациенты
      pthread_mutex_lock(&patients_to_specialist_lock);
      bool all_sent = (patients_to_specialist == N);
      pthread_mutex_unlock(&patients_to_specialist_lock);

      if (all_sent) {
        pthread_mutex_unlock(&commonQueueLock);
        goto end_duty;
      }

      pthread_cond_wait(&commonQueueNotEmpty, &commonQueueLock);
    }

    // Здесь очередь не пуста, берем пациента
    Patient *p = commonQueue.front();
    commonQueue.pop();
    pthread_mutex_unlock(&commonQueueLock);

    // Принимаем пациента
    log_event("Дежурный D%d принял пациента P%d\n", did, p->id);
    sleep(t_d);

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
    pthread_mutex_lock(&patients_to_specialist_lock);
    patients_to_specialist++;
    bool now_all_sent = (patients_to_specialist == N);
    pthread_mutex_unlock(&patients_to_specialist_lock);

    // Если теперь все пациенты направлены, разбудим все потоки, чтобы они
    // проверили свои условия
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
  int sid = *(int *)arg;
  delete (int *)arg;

  const char *specName = (sid == 0)   ? "Стоматолог"
                         : (sid == 1) ? "Хирург"
                                      : "Терапевт";

  while (true) {
    pthread_mutex_lock(&specialistLock[sid]);
    while (specialistQueue[sid].empty()) {
      pthread_mutex_lock(&patients_to_specialist_lock);
      bool all_sent = (patients_to_specialist == N);
      pthread_mutex_unlock(&patients_to_specialist_lock);

      if (all_sent && specialistQueue[sid].empty()) {
        // Все пациенты уже направлены и очередь пуста - выходим
        pthread_mutex_unlock(&specialistLock[sid]);
        goto end_specialist;
      }

      pthread_cond_wait(&specialistNotEmpty[sid], &specialistLock[sid]);
    }

    Patient *p = specialistQueue[sid].front();
    specialistQueue[sid].pop();
    pthread_mutex_unlock(&specialistLock[sid]);

    // Лечение пациента
    log_event("%s начал лечение пациента P%d\n", specName, p->id);
    sleep(t_s);
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
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_help();
      exit(0);
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

  if (from_file) {
    std::ifstream fin(config_filename.c_str());
    if (!fin) {
      std::cerr << "Не удалось открыть файл конфигурации\n";
      return false;
    }
    std::string line;
    while (std::getline(fin, line)) {
      if (line.find("n=") == 0) {
        N = atoi(line.substr(2).c_str());
      } else if (line.find("t_d=") == 0) {
        t_d = atoi(line.substr(4).c_str());
      } else if (line.find("t_s=") == 0) {
        t_s = atoi(line.substr(4).c_str());
      } else if (line.find("o=") == 0) {
        output_filename = line.substr(7);
      }
    }
  }

  log_parameters();
  return true;
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "ru");
  srand(time(NULL));

  if (!parse_args(argc, argv)) {
    std::cerr << "Ошибка при чтении параметров\n";
    return 1;
  }

  log_file = fopen(output_filename.c_str(), "w");
  if (!log_file) {
    std::cerr << "Не удалось открыть файл вывода\n";
    return 1;
  }

  // Инициализируем мьютексы для специалистов
  for (int i = 0; i < 3; i++) {
    pthread_mutex_init(&specialistLock[i], NULL);
    pthread_cond_init(&specialistNotEmpty[i], NULL);
  }

  // Создаем потоки дежурных врачей
  for (int i = 0; i < 2; i++) {
    int *id = new int(i + 1);
    pthread_create(&duty_docs[i], NULL, duty_doctor_thread, (void *)id);
  }

  // Создаем потоки специалистов
  for (int i = 0; i < 3; i++) {
    int *id = new int(i);
    pthread_create(&specialists[i], NULL, specialist_thread, (void *)id);
  }

  // Создаем потоки пациентов
  patients = new pthread_t[N];
  for (int i = 0; i < N; i++) {
    int *pid = new int(i + 1);
    pthread_create(&patients[i], NULL, patient_thread, (void *)pid);
  }

  // Ждем завершения всех потоков пациентов
  for (int i = 0; i < N; i++) {
    pthread_join(patients[i], NULL);
  }

  log_event("Все пациенты вылечены\n");

  for (int i = 0; i < 2; i++) {
    pthread_mutex_lock(&commonQueueLock);
    pthread_cond_broadcast(&commonQueueNotEmpty);
    pthread_mutex_unlock(&commonQueueLock);
  }

  // Все пациенты уже пришли и ушли. Дежурные закончат, когда направят всех N
  // пациентов. После чего завершим дежурных.
  for (int i = 0; i < 2; i++) {
    pthread_join(duty_docs[i], NULL);
  }

  // Это должно было уже выполниться при завершении всех потоков пациентов,
  // но... на всякий случай тут тоже убедимся.
  pthread_mutex_lock(&commonQueueLock);
  pthread_cond_broadcast(&commonQueueNotEmpty);
  pthread_mutex_unlock(&commonQueueLock);

  // Все дежурные завершились, значит patients_to_specialist == N.
  // Пробудим всех специалистов, если кто-то ещё спит.
  for (int i = 0; i < 3; i++) {
    pthread_mutex_lock(&specialistLock[i]);
    pthread_cond_broadcast(&specialistNotEmpty[i]);
    pthread_mutex_unlock(&specialistLock[i]);
  }

  // Ждем завершения всех специалистов
  for (int i = 0; i < 3; i++) {
    pthread_join(specialists[i], NULL);
  }

  log_event("Рабочий день в больнице завершен\n");
  
  // Удаляем мьютексы и условные переменные
  for (int i = 0; i < 3; i++) {
    pthread_mutex_destroy(&specialistLock[i]);
    pthread_cond_destroy(&specialistNotEmpty[i]);
  }
  pthread_mutex_destroy(&commonQueueLock);
  pthread_cond_destroy(&commonQueueNotEmpty);

  pthread_mutex_destroy(&consoleLogLock);
  pthread_mutex_destroy(&fileLogLock);
  pthread_mutex_destroy(&patients_to_specialist_lock);

  fclose(log_file);
  delete[] patients;

  return 0;
}
