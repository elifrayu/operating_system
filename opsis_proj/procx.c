#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <semaphore.h>
#include <pthread.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <string.h>
// Named shared memory / semaphore / message queue isimleri
#define SHM_NAME "/procx_shm"
#define SEM_NAME "/procx_sem"
#define MQ_NAME "/procx_mq"

#define MAX_PROCESSES 50
// Mesaj kuyruğu komut tipleri 1-başladı , 2- sonlandı
#define COMMAND_START 1
#define COMMAND_TERMINATE 2

// Process çalışma modu
typedef enum
{
    ATTACHED = 0,
    DETACHED = 1
} ProcessMode;
// Process durumu
typedef enum
{
    RUNNING = 0,
    TERMINATED = 1
} ProcessStatus;
// Shared memory’de tutulacak tek bir process kaydı
typedef struct
{
    pid_t pid;            // Çalışan process’in PID’si
    pid_t owner_pid;      // Process’i başlatan ProcX instance’ın PID’si
    char command[256];    // Çalıştırılan komut
    ProcessMode mode;     // attached/detached
    ProcessStatus status; // running/terminated
    time_t start_time;
    int is_active; // Kayıt aktif mi? 0 boş, 1 dolu
} ProcessInfo;
// Shared memory’de tutulacak tüm process kayıtları
typedef struct
{
    ProcessInfo processes[MAX_PROCESSES];
    int process_count;  // Aktif process sayısı
    int instance_count; // Açık ProcX instance sayısı(aynı shared memory kullanan terminal sayısı)
} SharedData;
// Mesaj kuyruğu mesaj yapısı
typedef struct
{
    long msg_type;    // Mesaj tipi (her zaman 1)
    int command;      // COMMAND_START veya COMMAND_TERMINATE
    pid_t sender_pid; // Mesajı gönderen ProcX PID’si
    pid_t target_pid; // Hedef process PID’si
} Message;

// Global değişkenler
// Shared memory için file descriptor, semaphore ve message
int shm_fd = -1;
sem_t *semaphore = SEM_FAILED;
mqd_t message_queue = (mqd_t)-1;
SharedData *shared_data = NULL;
// Threadler için tanımlamayıcılar
pthread_t monitor_thread;
pthread_t ipc_listener_thread;
// Programın ana döngüsünü kontrol eden flag
volatile sig_atomic_t is_running = 1;
// MENU-KULLANICI ARAYÜZÜ
void display_menu()
{
    printf("\n===== ProcX v1.0 =====\n");
    printf("1. Yeni Program Çalıştır\n");
    printf("2. Çalışan Programları Listele\n");
    printf("3. Program Sonlandır\n");
    printf("0. Çıkış\n");
    printf("Seçiminiz: ");
    fflush(stdout);
}
int remove_process(pid_t pid);
// IPC Kaynaklarını Başlat (Lab: shm_open, sem_open, mq_open) - IPC birden fazla processin birbiriyle iletişim kurmasını sağlar
int initialize_ipc_resources()
{
    // shared memory oluşturma
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1)
        return -1;

    if (ftruncate(shm_fd, sizeof(SharedData)) == -1)
        return -1;
    // ortak bellek eşleme
    shared_data = mmap(NULL, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_data == MAP_FAILED)
        return -1;
    // paylaşılan tablo içeriğini kilitlemek için semafor oluşturma
    semaphore = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (semaphore == SEM_FAILED)
        return -1;
    // race condition önleme için instance sayısını artırma
    sem_wait(semaphore);
    if (shared_data->instance_count == 0)
    {
        memset(shared_data, 0, sizeof(SharedData));
    }
    shared_data->instance_count++;
    sem_post(semaphore);

    struct mq_attr attr = {0};
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(Message);

    message_queue = mq_open(MQ_NAME, O_CREAT | O_RDWR | O_NONBLOCK, 0666, &attr);
    if (message_queue == (mqd_t)-1)
        return -1;
    return 0;
}
// SIGINT sinyal işleyicisi
static void handle_sigint(int sig)
{
    (void)sig;
    is_running = 0;
    printf("\n[HANDLER] SIGINT alındı. Güvenli çıkış başlıyor...\n");
    fflush(stdout);
}
// ProcX instance’ının IPC nesneleriyle olan bağlantısını kapatmak -unlink yok sadece ben kullanmıyorum diyor
void cleanup_ipc_close_only()
{
    if (shared_data)
        munmap(shared_data, sizeof(SharedData));
    shared_data = NULL;
    if (shm_fd != -1)
        close(shm_fd);
    shm_fd = -1;
    if (semaphore != SEM_FAILED)
        sem_close(semaphore);
    semaphore = SEM_FAILED;
    if (message_queue != (mqd_t)-1)
        mq_close(message_queue);
    message_queue = (mqd_t)-1;
}
// Tüm IPC nesnelerini sistemden silmek
void cleanup_ipc_unlink_all()
{
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_NAME);
    mq_unlink(MQ_NAME);
}
// Messaj kuyruğu ile IPC bildirimi gönderme
void send_ipc_notification(int command, pid_t target)
{
    Message msg = {.msg_type = 1, .command = command, .sender_pid = getpid(), .target_pid = target};
    // Başarısız olsa bile devam et
    mq_send(message_queue, (char *)&msg, sizeof(Message), 0);
}
// Shared memory’ye yeni process ekleme(SharedMemory+Semaphore)
int add_process(pid_t pid, ProcessMode mode, const char *cmd)
{
    // Tüm process tablosu paylaşılan olduğu için semafor ile koruyoruz.
    sem_wait(semaphore);
    if (shared_data->process_count >= MAX_PROCESSES)
    {
        sem_post(semaphore);
        return -1;
    }
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (!shared_data->processes[i].is_active)
        {
            ProcessInfo *p = &shared_data->processes[i];
            p->pid = pid;
            p->owner_pid = getpid();
            strncpy(p->command, cmd, 255);
            p->mode = mode;
            p->status = RUNNING;
            p->start_time = time(NULL);
            p->is_active = 1;
            shared_data->process_count++;
            break;
        }
    }
    sem_post(semaphore);
    return 0;
}
int remove_process(pid_t pid)
{
    sem_wait(semaphore);
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        ProcessInfo *p = &shared_data->processes[i];
        if (p->is_active && p->pid == pid)
        {
            p->is_active = 0;
            if (shared_data->process_count > 0)
                shared_data->process_count--;
            sem_post(semaphore);
            return 0;
        }
    }
    sem_post(semaphore);
    return -1;
}
// Çalışan processleri listeleme
void list_processes()
{
    sem_wait(semaphore);

    printf("\n--- ÇALIŞAN PROGRAMLAR ---\n");
    time_t now = time(NULL);
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        ProcessInfo *p = &shared_data->processes[i];
        if (p->is_active)
        {
            int elapsed = (int)difftime(now, p->start_time); // kaç saniyedir çalışıyor
            printf("PID: %-5d | Komut: %-15s | Mod: %s | Owner: %-5d | Süre: %d sn\n",
                   p->pid,
                   p->command,
                   p->mode == ATTACHED ? "ATTACHED" : "DETACHED",
                   p->owner_pid,
                   elapsed);

            count++;
        }
    }
    printf("Toplam: %d process\n", count);

    sem_post(semaphore);
}
// Yeni program başlatma
void start_new_program()
{
    char cmd[256];
    int mode_int;

    printf("Komut girin: ");
    if (fgets(cmd, sizeof(cmd), stdin) == NULL)
    {
        return;
    }
    cmd[strcspn(cmd, "\n")] = 0;

    if (strlen(cmd) == 0)
    {
        printf("[HATA] Komut boş olamaz.\n");
        return;
    }
    printf("Mod (0: Attached, 1: Detached): ");
    if (scanf("%d", &mode_int) != 1)
    {
        printf("[HATA] Geçersiz mod girdiniz.\n");
        while (getchar() != '\n')
            ;
        return;
    }
    while (getchar() != '\n')
        ;

    ProcessMode mode = (mode_int == 1) ? DETACHED : ATTACHED;

    char *argv[32];
    char temp[256];
    strcpy(temp, cmd);

    int i = 0;
    char *t = strtok(temp, " ");
    while (t && i < 31)
    {
        argv[i++] = t;
        t = strtok(NULL, " ");
    }
    argv[i] = NULL;

    // Child process oluşturma
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("[HATA] fork");
        return;
    }
    // CHILD PROCESS
    if (pid == 0)
    {
        if (mode == DETACHED)
        {
            // 1) Yeni session aç → terminalden kop
            if (setsid() == -1)
            {
                perror("setsid failed");
                exit(1);
            }
            // 2) Terminal kapanırsa ölmemek için SIGHUP ignore.
            signal(SIGHUP, SIG_IGN);
            // 3) Terminal I/O’dan kopart
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }
        // 4) Asıl programı çalıştır
        execvp(argv[0], argv);
        perror("execvp");
        exit(1);
    }
    // PARENT PROCESS
    // 1) Çocuk process’i shared memory’ye ekle
    if (add_process(pid, mode, argv[0]) == -1)
    {
        printf("[HATA] Process listesi dolu! (MAX_PROCESSES)\n");
        return;
    }
    // 2) Diğer instance’lara START bildirimi gönder
    send_ipc_notification(COMMAND_START, pid);
    // 3) SUCCESS mesajı
    printf("[SUCCESS] Process başlatıldı: PID %d | Komut: %s | Mod: %s\n",
           pid,
           argv[0],
           (mode == ATTACHED ? "ATTACHED" : "DETACHED"));
    // ATTACHED modda bu terminal yeni komut ALMAMALI -> child bitene kadar bekle
    if (mode == ATTACHED)
    {
        int status;

        while (waitpid(pid, &status, 0) == -1)
        {
            if (errno == EINTR)
                continue;
            perror("waitpid");
            break;
        }
        // Child bitti -> tablodan sil + TERMINATE olayı yayınla.
        remove_process(pid);
        send_ipc_notification(COMMAND_TERMINATE, pid);
        printf("[INFO] Attached process bitti: PID %d\n", pid);
    }
    fflush(stdout);
}
// Menüden process sonlandırma(Signal + kill)
void terminate_program()
{
    pid_t target_pid;

    printf("Sonlandırılacak PID: ");
    if (scanf("%d", &target_pid) != 1)
    {
        printf("[HATA] Geçersiz PID girişi.\n");
        while (getchar() != '\n')
            ;
        return;
    }
    while (getchar() != '\n')
        ;
    if (target_pid <= 0)
    {
        printf("[HATA] PID 0 veya negatif olamaz.\n");
        return;
    }
    // Shared tabloda PID var mı ve bu process bizim child mı?
    int found = 0;
    int is_my_child = 0;

    sem_wait(semaphore);
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        ProcessInfo *p = &shared_data->processes[i];
        if (p->is_active && p->pid == target_pid)
        {
            found = 1;
            if (p->owner_pid == getpid())
                is_my_child = 1;
            break;
        }
    }
    sem_post(semaphore);

    if (!found)
    {
        printf("[HATA] PID %d listede yok.\n", target_pid);
        return;
    }

    printf("[TERM] PID %d için sonlandırma isteği gönderiliyor...\n", target_pid);

    if (kill(target_pid, SIGTERM) == 0)
    {
        printf("[INFO] Process %d'e SIGTERM gönderildi.\n", target_pid);

        // Bizim child değilse: monitor bunu yakalayamaz → hemen temizle + IPC
        if (!is_my_child)
        {
            remove_process(target_pid);
            send_ipc_notification(COMMAND_TERMINATE, target_pid);
            printf("[INFO] Process %d (non-child) listeden temizlendi.\n", target_pid);
        }
        //  Bizim child ise: “sonlandı” mesajını monitor basacak, tabloyu da o temizleyecek.
    }
    else
    {
        if (errno == ESRCH)
        {
            printf("[INFO] PID %d zaten yok (ESRCH). Listeden temizleniyor...\n", target_pid);
            remove_process(target_pid);
            send_ipc_notification(COMMAND_TERMINATE, target_pid);
        }
        else
        {
            perror("[HATA] kill başarısız");
        }
    }
}
// Programdan çıkarken kendi başlattığı attached processleri sonlandır
void terminate_attached_processes()
{
    printf("[EXIT] ATTACHED process temizliği başlıyor...\n");
    fflush(stdout);
    sem_wait(semaphore);
    pid_t mypid = getpid();
    int killed = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        ProcessInfo *p = &shared_data->processes[i];

        if (p->is_active && p->owner_pid == mypid && p->mode == ATTACHED)
        {
            printf("[EXIT] ATTACHED kapatılıyor -> PID=%d Komut=%s\n", p->pid, p->command);
            fflush(stdout);
            kill(p->pid, SIGTERM);
            // Child ise zombie kalmasın diye waitpid
            // Eğer bu süreç benim child'ımsa: zombie kalmaması için waitpid ile reap et
            waitpid(p->pid, NULL, 0);
            p->is_active = 0;
            shared_data->process_count--;
            send_ipc_notification(COMMAND_TERMINATE, p->pid);
            killed++;
        }
    }
    sem_post(semaphore);
    printf("[EXIT] ATTACHED temizliği bitti. Kapatılan: %d\n", killed);
    fflush(stdout);
}
// Monitor thread fonksiyonu (zombie process kontrolü)
// Belirli aralıklarla çalışan processleri kontrol eder Zombie kalmasın diye
void *monitor_thread_func(void *arg)
{
    (void)arg;

    while (is_running)
    {
        sleep(2);

        pid_t mypid = getpid();

        //  Benim DETACHED child’larımı topla (sema altında kısa tut)
        pid_t my_detached[MAX_PROCESSES];
        int n = 0;

        sem_wait(semaphore);
        for (int i = 0; i < MAX_PROCESSES; i++)
        {
            ProcessInfo *p = &shared_data->processes[i];
            if (!p->is_active)
                continue;

            if (p->owner_pid == mypid && p->mode == DETACHED)
            {
                my_detached[n++] = p->pid; // child olma ihtimali en yüksek ve waitpid çalışır
            }
        }
        sem_post(semaphore);
        //  Bu PID’leri waitpid ile reaping (child oldukları için)
        for (int i = 0; i < n; i++)
        {
            int status;
            pid_t r = waitpid(my_detached[i], &status, WNOHANG);

            if (r == my_detached[i])
            {
                printf("\n[MONITOR] Process %d sonlandı.\n", r);
                remove_process(r);
                send_ipc_notification(COMMAND_TERMINATE, r);
            }
        }
        //  Hayalet temizliği: benim child’ım olmayan ama artık yaşamayan PID’leri sil
        sem_wait(semaphore);
        for (int i = 0; i < MAX_PROCESSES; i++)
        {
            ProcessInfo *p = &shared_data->processes[i];
            if (!p->is_active)
                continue;

            if (p->owner_pid != mypid)
            {
                if (kill(p->pid, 0) == -1 && errno == ESRCH)
                {
                    printf("\n[MONITOR] Hayalet temizlendi: PID %d\n", p->pid);
                    pid_t dead = p->pid;
                    p->is_active = 0;
                    if (shared_data->process_count > 0)
                        shared_data->process_count--;
                    send_ipc_notification(COMMAND_TERMINATE, dead);
                }
            }
        }
        sem_post(semaphore);
    }

    return NULL;
}

// IPC listener thread fonksiyonu (mesaj kuyruğu dinleme)
// Mesaj kuyruğunu dinler ve diğer ProcX instance’larından gelen bildirimleri işler START/TERMINATE
void *ipc_listener_thread_func(void *arg)
{
    (void)arg;
    Message msg;
    unsigned prio;

    while (is_running)
    {
        ssize_t n = mq_receive(message_queue,
                               (char *)&msg,
                               sizeof(msg),
                               &prio);

        if (n > 0)
        {
            if (msg.sender_pid != getpid())
            {
                printf("\n[IPC] Olay: PID %d -> %s\n",
                       msg.target_pid,
                       msg.command == COMMAND_START ? "START" : "TERMINATE");
            }
        }
        else
        {
            usleep(100000);
        }
    }
    return NULL;
}
// Main fonksiyonu
int main()
{
    // IPC kaynaklarını başlat (shared memory, semaphore, message queue)
    initialize_ipc_resources();
    // Ctrl+C (SIGINT) yakalanınca is_running=0 yapıp programı güvenli kapatacağız
    signal(SIGINT, handle_sigint);
    // Threadleri başlat
    pthread_create(&monitor_thread, NULL, monitor_thread_func, NULL);
    pthread_create(&ipc_listener_thread, NULL, ipc_listener_thread_func, NULL);

    int choice;
    // Ana döngü
    while (is_running)
    {
        display_menu();
        int rc = scanf("%d", &choice);
        if (rc != 1)
        {
            if (errno == EINTR)
            {
                // SIGINT geldi -> handler is_running=0 yaptı zaten
                clearerr(stdin);
                continue; // while(is_running) başına dön
            }
            // kullanıcı saçma giriş yaptıysa buffer temizle
            while (getchar() != '\n' && !feof(stdin))
            {
            }
            continue;
        }
        getchar();

        switch (choice)
        {
        case 1:
            start_new_program();
            break;
        case 2:
            list_processes();
            break;
        case 3:
            terminate_program();
            break;
        case 0:
            printf("\n[EXIT] Çıkış isteği alındı. Kapatma işlemleri başlıyor...\n");
            fflush(stdout);
            is_running = 0;
            break;
        default:
            printf("Geçersiz seçim!\n");
        }
    }
    // Çıkarken sadece bu instance’ın başlattığı ATTACHED process’leri öldür
    terminate_attached_processes();
    // Thread’leri düzgün kapat
    pthread_join(monitor_thread, NULL);
    // Tüm IPC Kaynaklarını temizle
    pthread_join(ipc_listener_thread, NULL);

    //"Son ProcX instance mı?" kontrolü (IPC isimlerini unlink etmek için)
    int last = 0;

    sem_wait(semaphore);
    shared_data->instance_count--;
    if (shared_data->instance_count <= 0)
        last = 1;
    sem_post(semaphore);

    // Her durumda: kendi handle'larımızı kapat (close/munmap)
    cleanup_ipc_close_only();

    // sadece son ProcX ise unlink
    if (last)
    {
        printf("[CLEANUP] Son ProcX kapaniyor. IPC kaynaklari siliniyor...\n");
        cleanup_ipc_unlink_all();
    }
    else
    {
        printf("[CLEANUP] Diger ProcX instance'lari acik. IPC kaynaklari korunuyor...\n");
    }

    return 0;
}
