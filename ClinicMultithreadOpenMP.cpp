#include <chrono> // ��� ������ � ��������
#include <cstdio> // ��� ������� �����-������ C (printf, fprintf)
#include <cstdlib> // ��� ������� ����������� ���������� C (atoi, rand � ��.)
#include <cstring> // ��� ������� ������ �� �������� C (strcmp)
#include <fstream> // ��� ������ � ������� (ifstream, ofstream)
#include <iostream> // ��� ������������ �����-������ C++ (cin, cout)
#include <omp.h> // ��� OpenMP
#include <queue> // ��� ���������� ������� (std::queue)
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
  bool treated; // ����, �����������, ������� �� �������
};

// ���������� ����������
int N = 5;      // ����� ���������
int t_d = 1000; // ����� ������ ��������� ����� (��)
int t_s = 2000; // ����� ������ ����������� (��)
std::string output_filename = "clinic_log.txt"; // ��� ����� ��� ������ �����
bool from_file = false; // ���� ������ ���������� �� �����
std::string config_filename; // ��� ����� ������������

// ������� � ��������
std::queue<Patient *> commonQueue; // ������� ��������� � �������� ������
omp_lock_t commonQueueLock; // ���������� ��� ������� ��������

// ������� � ������������: ����������(0), ������(1), ��������(2)
std::queue<Patient *> specialistQueue[3]; // ��� ������� ��� ���� ������������
omp_lock_t specialistLock[3]; // ���������� ��� ������ ������� ������������

// Spinlocks ��� ����������� � ��������
omp_lock_t consoleLogLock; // ���������� ��� ����������� � �������
omp_lock_t fileLogLock; // ���������� ��� ����������� � ����
omp_lock_t patientsToSpecialistLock; // ���������� ��� ������� � ��������

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

  // ����� � ������� � �������������� ����������
  omp_set_lock(&consoleLogLock);
  printf("%s ", time_str.c_str());
  vprintf(fmt, args);
  omp_unset_lock(&consoleLogLock);

  // ����� � ���� � �������������� ����������
  if (log_file) {
    omp_set_lock(&fileLogLock);
    fprintf(log_file, "%s ", time_str.c_str());
    vfprintf(log_file, fmt, args2);
    omp_unset_lock(&fileLogLock);
    va_end(args2);
  }
  va_end(args);
}

// ������� �������� �������� � ���������� � ������� � ��������
void create_patient(int pid) {
  Patient *p = new Patient();
  p->id = pid;
  p->specialist_type = NONE;
  p->treated = false;

  // ��������� �������� � ������� � ��������
  omp_set_lock(&commonQueueLock);
  commonQueue.push(p);
  log_event("Patient P%d entered the queue to the duty doctors\n", p->id);
  omp_unset_lock(&commonQueueLock);
}

// ������� ��������� �������� �������� ������
void duty_doctor(int did) {
  while (true) {
    Patient *p = nullptr;

    // ������� ����� �������� �� �������
    omp_set_lock(&commonQueueLock);
    if (!commonQueue.empty()) {
      p = commonQueue.front();
      commonQueue.pop();
    }
    omp_unset_lock(&commonQueueLock);

    if (p != nullptr) {
      // ��������� ��������
      log_event("Duty Doctor D%d accepted patient P%d\n", did, p->id);
      sleep_ms(t_d); // ��������� ����� ������

      // ���������� �����������
      p->specialist_type = static_cast<SpecialistType>(specialist_dist(rng));
      const char *specName = (p->specialist_type == DENTIST)   ? "Dentist"
                             : (p->specialist_type == SURGEON) ? "Surgeon"
                                                               : "Therapist";
      log_event("Duty Doctor D%d referred patient P%d to %s\n", did, p->id,
                specName);

      // ��������� �������� � ������� � �����������
      omp_set_lock(&specialistLock[p->specialist_type]);
      specialistQueue[p->specialist_type].push(p);
      omp_unset_lock(&specialistLock[p->specialist_type]);

      // ����������� ������� ������������ ���������
      omp_set_lock(&patientsToSpecialistLock);
      patientsToSpecialist++;
      bool now_all_sent = (patientsToSpecialist == N);
      omp_unset_lock(&patientsToSpecialistLock);

      // ���� ��� �������� ����������, ��������� ������
      if (now_all_sent) {
        break;
      }
    } else {
      // ���� ������� �����, ���������, ��� �� �������� ����������
      omp_set_lock(&patientsToSpecialistLock);
      bool all_sent = (patientsToSpecialist == N);
      omp_unset_lock(&patientsToSpecialistLock);

      if (all_sent) {
        break; // ��������� ������ �����
      } else {
        // ���� ������� ����� ��������� ���������
        sleep_ms(100); // 100 ��
      }
    }
  }

  log_event("Duty Doctor D%d ended his workday\n", did);
}

// ������� ��������� �������� ������������
void specialist(int sid) {
  const char *specName = (sid == DENTIST)   ? "Dentist"
                         : (sid == SURGEON) ? "Surgeon"
                                            : "Therapist";

  while (true) {
    Patient *p = nullptr;

    // ������� ����� �������� �� ������� �����������
    omp_set_lock(&specialistLock[sid]);
    if (!specialistQueue[sid].empty()) {
      p = specialistQueue[sid].front();
      specialistQueue[sid].pop();
    }
    omp_unset_lock(&specialistLock[sid]);

    if (p != nullptr) {
      // ������� ��������
      log_event("%s started treating patient P%d\n", specName, p->id);
      sleep_ms(t_s); // ��������� ����� �������
      log_event("%s finished treating patient P%d\n", specName, p->id);

      // ���������� ��������
      p->treated = true;
    } else {
      // ���� ������� �����, ���������, ��� �� �������� ����������
      omp_set_lock(&patientsToSpecialistLock);
      bool all_sent = (patientsToSpecialist == N);
      omp_unset_lock(&patientsToSpecialistLock);

      if (all_sent) {
        break; // ��������� ������ �����������
      } else {
        // ���� ������� ����� ��������� ���������
        sleep_ms(100); // 100 ��
      }
    }
  }

  log_event("%s ended his workday\n", specName);
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
  log_event("Simulation Parameters:\n");
  log_event("Number of patients: %d\n", N);
  log_event("Duty doctor's processing time (ms): %d\n", t_d);
  log_event("Specialist's treatment time (ms): %d\n", t_s);
  log_event("Log file: %s\n\n", output_filename.c_str());
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
      std::cerr << "Failed to open configuration file\n";
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
        output_filename = line.substr(2);
      }
    }
  }

  return true;
}

int main(int argc, char **argv) {
  setlocale(LC_ALL, "ru"); // ������������� ������ (������� ����)

  // �������������� ����������
  omp_init_lock(&commonQueueLock);
  for (int i = 0; i < 3; i++) {
    omp_init_lock(&specialistLock[i]);
  }
  omp_init_lock(&consoleLogLock);
  omp_init_lock(&fileLogLock);
  omp_init_lock(&patientsToSpecialistLock);

  // ������ ��������� ��������� ������ ��� ����� ������������
  if (!parse_args(argc, argv)) {
    std::cerr << "Error reading parameters\n";
    return 1;
  }

  // ��������� ���� ����� �� ������
  log_file = fopen(output_filename.c_str(), "w+");
  if (!log_file) {
    std::cerr << "Failed to open output file\n";
    return 1;
  }

  log_parameters(); // �������� ��������� ������

// ��������� ������������ ������
#pragma omp parallel
  {
#pragma omp single
    {
      // ������� ������ �������� ������
      for (int i = 0; i < 2; i++) {
        int did = i + 1;
#pragma omp task firstprivate(did)
        { duty_doctor(did); }
      }

      // ������� ������ ������������
      for (int i = 0; i < 3; i++) {
        int sid = i;
#pragma omp task firstprivate(sid)
        { specialist(sid); }
      }

      // ������� ������ ���������
      for (int i = 0; i < N; i++) {
        int pid = i + 1;
#pragma omp task firstprivate(pid)
        { create_patient(pid); }
      }
    }
  } // ����� ������������� �������

  log_event(
      "The hospital workday has ended\n"); // �������� ���������� �������� ���

  // ���������� ����������
  omp_destroy_lock(&commonQueueLock);
  for (int i = 0; i < 3; i++) {
    omp_destroy_lock(&specialistLock[i]);
  }
  omp_destroy_lock(&consoleLogLock);
  omp_destroy_lock(&fileLogLock);
  omp_destroy_lock(&patientsToSpecialistLock);

  fclose(log_file); // ��������� ���� �����

  return 0; // �������� ���������� ���������
}
