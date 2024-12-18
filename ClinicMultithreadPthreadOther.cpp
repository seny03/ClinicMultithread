#include <chrono> // ��� ������ � ��������
#include <cstdio> // ��� ������� �����-������ C (printf, fprintf)
#include <cstdlib> // ��� ������� ����������� ���������� C (atoi, rand � ��.)
#include <cstring> // ��� ������� ������ �� �������� C (strcmp)
#include <fstream> // ��� ������ � ������� (ifstream, ofstream)
#include <iostream> // ��� ������������ �����-������ C++ (cin, cout)
#include <pthread.h> // ��� ������ � �������� POSIX (pthread_*)
#include <queue>     // ��� ���������� ������� (std::queue)
#include <random> // ��� ����������� ��������� ����� (std::mt19937)
#include <stdarg.h> // ��� ������ � variadic ����������� (va_list)
#include <string> // ��� ������ std::string

#if _WIN32
#include <windows.h> // ��� ������� Sleep �� Windows
#define sleep_ms(x) Sleep(x) // ���������� sleep_ms ��� Sleep (������������)
#else
#include <unistd.h> // ��� ������� usleep �� UNIX/Linux
#define sleep_ms(x)                                                            \
  usleep(1000L * (x)) // ���������� sleep_ms ����� usleep (������������)
#endif

// ������������ ����� ������������
enum SpecialistType { NONE = -1, DENTIST = 0, SURGEON = 1, THERAPIST = 2 };

// ��������� ��������
struct Patient {
  int id; // ������������� ��������
  SpecialistType specialist_type; // ��� �����������, � �������� ���������
  pthread_cond_t treated =
      PTHREAD_COND_INITIALIZER; // �������� ���������� ��� �������� �������
  pthread_mutex_t patientLock; // ������� ��� ������������� ��������� ��������
};

// ���������� ����������
int N = 5;      // ����� ���������
int t_d = 1000; // ����� ������ ��������� ����� (��)
int t_s = 2000; // ����� ������ ����������� (��)
std::string output_filename = "clinic_log.txt"; // ��� ����� ��� ������ �����
bool from_file = false; // ���� ������ ���������� �� �����
std::string config_filename; // ��� ����� ������������

// ������
pthread_t *patients; // ������ ������� ���������
pthread_t duty_docs[2]; // ������ ������� �������� ������ (2 �����)
pthread_t specialists[3]; // ������ ������� ������������ (3 �����������)

// ������� � ��������
std::queue<Patient *> commonQueue; // ������� ��������� � �������� ������
pthread_mutex_t commonQueueLock; // ������� ��� ������� ��������
pthread_cond_t commonQueueNotEmpty =
    PTHREAD_COND_INITIALIZER; // �������� ���������� ��� ���������� � �����
                              // ���������

// ������� � ������������: ����������(0), ������(1), ��������(2)
std::queue<Patient *> specialistQueue[3]; // ��� ������� ��� ���� ������������
pthread_mutex_t specialistLock[3]; // �������� ��� ������ ������� ������������
pthread_cond_t
    specialistNotEmpty[3]; // �������� ���������� ��� �������� ������������

// Spinlocks ��� ����������� � ��������
pthread_spinlock_t consoleLogLock; // Spinlock ��� ����������� � �������
pthread_spinlock_t fileLogLock; // Spinlock ��� ����������� � ����
pthread_spinlock_t patientsToSpecialistLock; // Spinlock ��� ������� � ��������

// ������� ������������ ���������
int patientsToSpecialist = 0;

// ���� ������
FILE *log_file = NULL; // ��������� �� ���� �����
auto program_start =
    std::chrono::high_resolution_clock::now(); // ����� ������ ���������

// ��������� ��������� �����
std::mt19937
    rng(42); // ����������� ��������� ��������� ����� � ������������� �����
std::uniform_int_distribution<int>
    specialist_dist(0, 2); // ������������� ��� ������ �����������

// ���������� ������� ��� ���������� ���������
pthread_mutexattr_t adaptive_attr;

// ������� ��� ��������� ������� � ������� ������ ���������
std::string get_time_since_start() {
  auto now = std::chrono::high_resolution_clock::now(); // ������� �����
  auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - program_start)
          .count();                    // ��������� ����� � ��
  int minutes = elapsed / 60000;       // ��������� � ������
  int seconds = (elapsed / 1000) % 60; // ������� � ��������
  int milliseconds = elapsed % 1000;   // ������������
  char buffer[30];
  sprintf(buffer, "[%02d:%02d:%03d]", minutes, seconds,
          milliseconds); // ����������� ������ �������
  return std::string(buffer);
}

// ������� �����������
void log_event(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  va_list args2;
  va_copy(args2, args);

  std::string time_str = get_time_since_start();

  // ����� � ������� � �������������� spinlock
  pthread_spin_lock(&consoleLogLock);
  printf("%s ", time_str.c_str());
  vprintf(fmt, args);
  pthread_spin_unlock(&consoleLogLock);

  // ����� � ���� � �������������� spinlock
  if (log_file) {
    pthread_spin_lock(&fileLogLock);
    fprintf(log_file, "%s ", time_str.c_str());
    vfprintf(log_file, fmt, args2);
    pthread_spin_unlock(&fileLogLock);
    va_end(args2);
  }
  va_end(args);
}

// ����� ��������
void *patient_thread(void *arg) {
  int pid = *(int *)arg; // ��������� id �������� �� ���������
  delete (int *)arg; // ����������� ������ ��� id

  Patient *p = new Patient(); // ������� ����� ������ ��������
  p->id = pid;
  p->specialist_type = NONE;

  // �������������� ���������� ������� ��������
  pthread_mutex_init(&p->patientLock, &adaptive_attr);

  // ��������� �������� � ������� � ��������
  pthread_mutex_lock(&commonQueueLock);
  commonQueue.push(p);
  log_event("������� P%d ����� � ������� � ��������\n", p->id);
  pthread_cond_signal(&commonQueueNotEmpty);
  pthread_mutex_unlock(&commonQueueLock);

  // ����, ���� ������� ����� �������
  pthread_mutex_lock(&p->patientLock);
  pthread_cond_wait(&p->treated, &p->patientLock);
  pthread_mutex_unlock(&p->patientLock);

  log_event("������� P%d ��������� ������� � ����� �����\n", p->id);
  delete p; // ����������� ������ ��� ��������
  return NULL;
}

// ����� ��������� �����
void *duty_doctor_thread(void *arg) {
  int did = *(int *)arg; // ��������� id ��������� �����
  delete (int *)arg;     // ����������� ������ ��� id

  while (true) {
    pthread_mutex_lock(&commonQueueLock);

    while (commonQueue.empty()) {
      // ���������, �� ���������� �� ��� ��������
      pthread_spin_lock(&patientsToSpecialistLock);
      bool all_sent = (patientsToSpecialist == N);
      pthread_spin_unlock(&patientsToSpecialistLock);

      if (all_sent) {
        pthread_mutex_unlock(&commonQueueLock);
        goto end_duty; // ��������� ������ �����
      }

      pthread_cond_wait(&commonQueueNotEmpty, &commonQueueLock);
    }

    // �������� �������� �� �������
    Patient *p = commonQueue.front();
    commonQueue.pop();
    pthread_mutex_unlock(&commonQueueLock);

    // ��������� ��������
    log_event("�������� D%d ������ �������� P%d\n", did, p->id);
    sleep_ms(t_d); // ��������� ����� ������

    // ���������� �����������
    p->specialist_type = static_cast<SpecialistType>(specialist_dist(rng));
    const char *specName = (p->specialist_type == DENTIST) ? "�����������"
                           : (p->specialist_type == SURGEON) ? "�������"
                                                             : "���������";
    log_event("�������� D%d �������� �������� P%d � %s\n", did, p->id,
              specName);

    // ��������� �������� � ������� � �����������
    pthread_mutex_lock(&specialistLock[p->specialist_type]);
    specialistQueue[p->specialist_type].push(p);
    pthread_cond_signal(&specialistNotEmpty[p->specialist_type]);
    pthread_mutex_unlock(&specialistLock[p->specialist_type]);

    // ����������� ������� ������������ ���������
    pthread_spin_lock(&patientsToSpecialistLock);
    patientsToSpecialist++;
    bool now_all_sent = (patientsToSpecialist == N);
    pthread_spin_unlock(&patientsToSpecialistLock);

    // ���� ��� �������� ����������, �������� ���� �������� � ������������
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
  log_event("�������� D%d �������� ���� ������� ����\n", did);
  return NULL;
}

// ����� �����������
void *specialist_thread(void *arg) {
  int sid = *(int *)arg; // ��������� id �����������
  delete (int *)arg;     // ����������� ������ ��� id

  const char *specName = (sid == DENTIST)   ? "����������"
                         : (sid == SURGEON) ? "������"
                                            : "��������";

  while (true) {
    pthread_mutex_lock(&specialistLock[sid]);

    while (specialistQueue[sid].empty()) {
      // ���������, ��� �� �������� ����������
      pthread_spin_lock(&patientsToSpecialistLock);
      bool all_sent = (patientsToSpecialist == N);
      pthread_spin_unlock(&patientsToSpecialistLock);

      if (all_sent && specialistQueue[sid].empty()) {
        pthread_mutex_unlock(&specialistLock[sid]);
        goto end_specialist; // ��������� ������ �����������
      }

      pthread_cond_wait(&specialistNotEmpty[sid], &specialistLock[sid]);
    }

    // �������� �������� �� �������
    Patient *p = specialistQueue[sid].front();
    specialistQueue[sid].pop();
    pthread_mutex_unlock(&specialistLock[sid]);

    // ������� ��������
    log_event("%s ����� ������� �������� P%d\n", specName, p->id);
    sleep_ms(t_s); // ��������� ����� �������
    log_event("%s �������� ������� �������� P%d\n", specName, p->id);

    // ���������� ��������
    pthread_mutex_lock(&p->patientLock);
    pthread_cond_signal(&p->treated);
    pthread_mutex_unlock(&p->patientLock);
  }

end_specialist:
  log_event("%s �������� ���� ������� ����\n", specName);
  return NULL;
}

// ������� ����������� �������
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

// ������� ����������� �������� ��� ��������� ����������
void log_parameters() {
  log_event("��������� ������:\n");
  log_event("���������� ���������: %d\n", N);
  log_event("����� ������ ��������� ����� (��): %d\n", t_d);
  log_event("����� ������ ����������� (��): %d\n", t_s);
  log_event("���� ��� ������ �����: %s\n\n", output_filename.c_str());
}

// ������� �������� ��������� ������ ��� �����
bool parse_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++) { // ���� �� ���� ���������� ��������� ������
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_help(); // ���� ��������� ������, ������� ��
      exit(0);      // � �������
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

  if (from_file) { // ���� ����� ������ �� ����� ������������
    std::ifstream fin(config_filename.c_str());
    if (!fin) {
      std::cerr << "�� ������� ������� ���� ������������\n";
      return false;
    }
    std::string line;
    while (std::getline(fin, line)) { // ������ ���������
      if (line.find("n=") == 0) {
        N = atoi(line.substr(2).c_str());
      } else if (line.find("t_d=") == 0) {
        t_d = atoi(line.substr(4).c_str());
      } else if (line.find("t_s=") == 0) {
        t_s = atoi(line.substr(4).c_str());
      } else if (line.find("o=") == 0) {
        output_filename =
            line.substr(2); // ����������: substr(2) ������ substr(7)
      }
    }
  }

  return true;
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "ru"); // ������������� ������ (������� ����)
  srand(time(NULL)); // �������������� ��������� ��������� �����

  // �������������� ������� ��� ���������� ���������
  pthread_mutexattr_init(&adaptive_attr);
  pthread_mutexattr_settype(&adaptive_attr, PTHREAD_MUTEX_ADAPTIVE_NP);

  // �������������� �������� ��� ������������ � ���������� �����
  for (int i = 0; i < 3; i++) {
    pthread_mutex_init(
        &specialistLock[i],
        &adaptive_attr); // �������������� ������� ����������� ��� ����������
    pthread_cond_init(&specialistNotEmpty[i],
                      NULL); // �������������� �������� ���������� �����������
  }

  // �������������� ������� ������� �������� ��� ����������
  pthread_mutex_init(&commonQueueLock, &adaptive_attr);

  // �������������� spinlock'�
  pthread_spin_init(&consoleLogLock, 0); // �������������� spinlock �������
  pthread_spin_init(&fileLogLock, 0); // �������������� spinlock �����
  pthread_spin_init(&patientsToSpecialistLock,
                    0); // �������������� spinlock ��������

  // ������ ��������� ��������� ������ ��� ����� ������������
  if (!parse_args(argc, argv)) {
    std::cerr << "������ ��� ������ ����������\n";
    return 1;
  }

  // ��������� ���� ����� �� ������
  log_file = fopen(output_filename.c_str(), "w+");
  if (!log_file) {
    std::cerr << "�� ������� ������� ���� ������\n";
    return 1;
  }

  log_parameters(); // �������� ��������� ������

  // ������� ������ �������� ������
  for (int i = 0; i < 2; i++) {
    int *id = new int(i + 1); // �������� ������ ��� id �����
    pthread_create(&duty_docs[i], NULL, duty_doctor_thread,
                   (void *)id); // ������� ����� ��������� �����
  }

  // ������� ������ ������������
  for (int i = 0; i < 3; i++) {
    int *id = new int(i); // �������� ������ ��� id �����������
    pthread_create(&specialists[i], NULL, specialist_thread,
                   (void *)id); // ������� ����� �����������
  }

  // ������� ������ ���������
  patients = new pthread_t[N]; // �������� ������ ��� ������ ������� ���������
  for (int i = 0; i < N; i++) {
    int *pid = new int(i + 1); // �������� ������ ��� id ��������
    pthread_create(&patients[i], NULL, patient_thread,
                   (void *)pid); // ������� ����� ��������
  }

  // ���� ���������� ���� ������� ���������
  for (int i = 0; i < N; i++) {
    pthread_join(patients[i], NULL); // ���� ���������� ������ ��������
  }

  log_event(
      "��� �������� ��������\n"); // �������� ���������� ������� ���� ���������

  // �������� �������� ������, ����� ��� ����� ��������� ������
  for (int i = 0; i < 2; i++) {
    pthread_mutex_lock(&commonQueueLock);
    pthread_cond_broadcast(&commonQueueNotEmpty); // ����� ���� �������� ������
    pthread_mutex_unlock(&commonQueueLock);
  }

  // ���� ���������� ���� ������� �������� ������
  for (int i = 0; i < 2; i++) {
    pthread_join(duty_docs[i], NULL);
  }

  // �������� ������������, ����� ��� ����� ��������� ������, ���� ��� ��������
  // ����������
  for (int i = 0; i < 3; i++) {
    pthread_mutex_lock(&specialistLock[i]);
    pthread_cond_broadcast(&specialistNotEmpty[i]); // ����� ������������
    pthread_mutex_unlock(&specialistLock[i]);
  }

  // ���� ���������� ���� ������� ������������
  for (int i = 0; i < 3; i++) {
    pthread_join(specialists[i], NULL);
  }

  log_event(
      "������� ���� � �������� ��������\n"); // �������� ���������� �������� ���

  // ������� �������� � �������� ����������
  for (int i = 0; i < 3; i++) {
    pthread_mutex_destroy(&specialistLock[i]); // ���������� ������� �����������
    pthread_cond_destroy(
        &specialistNotEmpty[i]); // ���������� �������� ���������� �����������
  }
  pthread_mutex_destroy(
      &commonQueueLock); // ���������� ������� ������� ��������
  pthread_cond_destroy(
      &commonQueueNotEmpty); // ���������� �������� ���������� ������� ��������

  // ���������� spinlock'��
  pthread_spin_destroy(&consoleLogLock); // ���������� spinlock �������
  pthread_spin_destroy(&fileLogLock); // ���������� spinlock �����
  pthread_spin_destroy(
      &patientsToSpecialistLock); // ���������� spinlock ��������

  // ���������� ���������� ���������
  pthread_mutexattr_destroy(&adaptive_attr);

  fclose(log_file);  // ��������� ���� �����
  delete[] patients; // ����������� ������ ��� ������ ������� ���������

  return 0; // �������� ���������� ���������
}
