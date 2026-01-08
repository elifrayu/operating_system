# ProcX - Gelişmiş Süreç Yönetim Sistemi

**Ders:** İşletim Sistemleri Dönem Projesi  
**Teslim Tarihi:** 16 Aralık 2024  
**Hazırlayan:** [ElIF UYAR ]

---

## Proje Hakkında
**ProcX**, Linux ortamında çalışan, çoklu terminal desteği olan bir süreç (process) yönetim sistemidir. Kullanıcıların komut çalıştırmasına, çalışan süreçleri listelemesine ve sonlandırmasına olanak tanır.

Bu proje, hazır kütüphaneler yerine **İşletim Sistemleri Laboratuvarı** müfredatında öğretilen düşük seviyeli sistem çağrıları (System Calls) kullanılarak geliştirilmiştir.

---

## Teknik Altyapı ve Lab Eşleştirmeleri

Projenin her modülü, dönem boyunca işlenen belirli bir laboratuvar föyüne dayanmaktadır.

| Modül | Kullanılan Yöntem
|---|---|---|---|
| **Süreç Yönetimi** | `fork`, `execvp`
 **Zombi Önleme** | `waitpid(WNOHANG)` 
| **Daemon Modu** | `setsid()` 
| **Ortak Hafıza** | `shm_open`, `mmap` (POSIX)
| **Mesajlaşma** | `mq_open`, `mq_send` 
| **Senkronizasyon** | `sem_open`, `sem_wait`
| **Thread Yapısı** | `pthread_create/join`

| **Güvenlik** | `kill(SIGTERM)`

---

##  Kurulum ve Çalıştırma

Proje `Makefile` yapısı ile yönetilmektedir.

**1. Derleme:**
Terminalde proje dizinine giderek aşağıdaki komutu çalıştırın:
```bash
make
