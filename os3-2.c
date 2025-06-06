#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGESIZE (32)
#define PAS_FRAMES (256) 
#define PAS_SIZE (PAGESIZE * PAS_FRAMES)
#define VAS_PAGES (64)
#define PTE_SIZE (4)
#define PAGE_INVALID (0)
#define PAGE_VALID (1)
#define MAX_REFERENCES (256)
#define MAX_PROCESSES (10)
#define L1_PT_ENTRIES (8)
#define L2_PT_ENTRIES (8)

typedef struct {
    unsigned char frame;
    unsigned char vflag;
    unsigned char ref;
    unsigned char pad;
} pte;

typedef struct {
    int pid;
    int ref_len;
    unsigned char *references;
    pte *L1_page_table;
    int page_faults;
    int ref_count;
} process;

unsigned char pas[PAS_SIZE];
int allocated_frame_count = 0;

int allocate_frame() {
    if (allocated_frame_count >= PAS_FRAMES) 
        return -1;
    return allocated_frame_count++;
}

// 페이지 테이블 프레임을 하나 할당하고, 해당 프레임을 0으로 초기화하여 반환하는 함수
// 2단계 페이지 테이블 구조에서 1단계/2단계 모두 8개 엔트리만 필요하므로 프레임 하나만 할당
// 반환값: 할당된 페이지 테이블의 시작 주소(실패 시 NULL)
pte *allocate_pagetable_frame() {
    int frame = allocate_frame(); // 사용 가능한 프레임 번호 할당
    if (frame == -1) 
        return NULL; // 프레임 할당 실패 시 NULL 반환
    pte *page_table_ptr = (pte *)&pas[frame * PAGESIZE]; // 프레임 시작 주소를 pte 포인터로 변환
    memset(page_table_ptr, 0, PAGESIZE); // 해당 프레임(32B)을 0으로 초기화
    return page_table_ptr; // 페이지 테이블 포인터 반환
}

int load_process(FILE *fp, process *proc) {
    if (fread(&proc->pid, sizeof(int), 1, fp) != 1) 
        return 0;
    if (fread(&proc->ref_len, sizeof(int), 1, fp) != 1) 
        return 0;
    proc->references = malloc(proc->ref_len);
    if (fread(proc->references, 1, proc->ref_len, fp) != proc->ref_len) 
        return 0;

    printf("%d %d\n", proc->pid, proc->ref_len);
    for (int i = 0; i < proc->ref_len; i++) {
        printf("%02d ", proc->references[i]);
    }
    printf("\n");

    proc->page_faults = 0;
    proc->ref_count = 0;
    if ((proc->L1_page_table = allocate_pagetable_frame()) == NULL)
        return -1;
    return 1;
}
void simulate(process *procs, int proc_count) {
    printf("simulate() start\n");

    int max_ref = 0;
    for (int i = 0; i < proc_count; i++) {
        if (procs[i].ref_len > max_ref)
            max_ref = procs[i].ref_len;
    }

    for (int idx = 0; idx < max_ref; idx++) {
        for (int i = 0; i < proc_count; i++) {
            process *p = &procs[i];
            if (idx >= p->ref_len) continue;

            unsigned char page = p->references[idx];
            p->ref_count++;

            int l1_idx = page / L2_PT_ENTRIES;
            int l2_idx = page % L2_PT_ENTRIES;

            pte *l1_entry = &p->L1_page_table[l1_idx];

            if (l1_entry->vflag == PAGE_INVALID) {
                pte *l2_pt = allocate_pagetable_frame();
                if (!l2_pt) {
                    printf("Out of memory!!\n");
                    return;
                }
                int l1_frame = (int)( ((unsigned char*)l2_pt - pas) / PAGESIZE );
                l1_entry->vflag = PAGE_VALID;
                l1_entry->frame = (unsigned char)l1_frame;
                p->page_faults++;

                int data_frame = allocate_frame();
                if (data_frame == -1) {
                    printf("Out of memory!!\n");
                    return;
                }

                pte *l2_entry = &l2_pt[l2_idx];
                l2_entry->vflag = PAGE_VALID;
                l2_entry->frame = (unsigned char)data_frame;
                l2_entry->ref = 1;
                p->page_faults++;

                printf("[PID %02d IDX:%03d] Page access %03d: (L1PT) PF -> Allocated Frame %03d(PTE %03d), (L2PT) PF -> Allocated Frame %03d\n",
                       p->pid, idx, page, l1_frame, l1_idx, data_frame);
            } else {
                int l1_frame = l1_entry->frame;
                pte *l2_pt = (pte*)&pas[l1_frame * PAGESIZE];
                pte *l2_entry = &l2_pt[l2_idx];

                if (l2_entry->vflag == PAGE_INVALID) {
                    int data_frame = allocate_frame();
                    if (data_frame == -1) {
                        printf("Out of memory!!\n");
                        return;
                    }

                    l2_entry->vflag = PAGE_VALID;
                    l2_entry->frame = (unsigned char)data_frame;
                    l2_entry->ref = 1;
                    p->page_faults++;

                    printf("[PID %02d IDX:%03d] Page access %03d: (L1PT) Frame %03d, (L2PT) PF -> Allocated Frame %03d\n",
                           p->pid, idx, page, l1_frame, data_frame);
                } else {
                    l2_entry->ref++;
                    printf("[PID %02d IDX:%03d] Page access %03d: (L1PT) Frame %03d, (L2PT) Frame %03d\n",
                           p->pid, idx, page, l1_frame, l2_entry->frame);
                }
            }
        }
    }

    printf("simulate() end\n");
}


void print_page_tables(process *procs, int proc_count) {
    int total_frames = allocated_frame_count;
    int total_faults = 0;
    int total_refs = 0;

    for (int i = 0; i < proc_count; i++) {
        process *p = &procs[i];
        total_faults += p->page_faults;
        total_refs += p->ref_count;

        int proc_frames = 0;

        printf("** Process %03d: Allocated Frames=", p->pid);

        for (int l1 = 0; l1 < L1_PT_ENTRIES; l1++) {
            pte *l1_entry = &p->L1_page_table[l1];
            if (l1_entry->vflag == PAGE_INVALID) continue;

            proc_frames++; // L2 page table 프레임
            int l1_frame = l1_entry->frame;

            pte *l2_pt = (pte*)&pas[l1_frame * PAGESIZE];
            for (int l2 = 0; l2 < L2_PT_ENTRIES; l2++) {
                pte *l2_entry = &l2_pt[l2];
                if (l2_entry->vflag == PAGE_VALID) {
                    proc_frames++;
                }
            }
        }

        printf("%03d PageFaults/References=%03d/%03d\n",
               proc_frames, p->page_faults, p->ref_count);

        for (int l1 = 0; l1 < L1_PT_ENTRIES; l1++) {
            pte *l1_entry = &p->L1_page_table[l1];
            if (l1_entry->vflag == PAGE_INVALID) continue;

            int l1_frame = l1_entry->frame;
            printf("(L1PT) [PTE] %03d -> [FRAME] %03d\n", l1, l1_frame);

            pte *l2_pt = (pte*)&pas[l1_frame * PAGESIZE];
            for (int l2 = 0; l2 < L2_PT_ENTRIES; l2++) {
                pte *l2_entry = &l2_pt[l2];
                if (l2_entry->vflag == PAGE_VALID) {
                    int page_num = l1 * L2_PT_ENTRIES + l2;
                    printf("(L2PT) [PAGE] %03d -> [FRAME] %03d REF=%03d\n",
                           page_num, l2_entry->frame, l2_entry->ref);
                }
            }
        }
    }

    printf("Total: Allocated Frames=%03d Page Faults/References=%03d/%03d\n",
           total_frames, total_faults, total_refs);
}

int main() {
    process procs[MAX_PROCESSES];
    int count = 0;

    printf("load_process() start\n");
    while (count < MAX_PROCESSES) {
        int ret = load_process(stdin, &procs[count]);
        if (ret == 0) 
            break;
        if (ret == -1) {
            printf("Out of memory!!\n");
            return 1;
        }
        count++;
    }
    printf("load_process() end\n");

    simulate(procs, count);
    print_page_tables(procs, count);

    for (int i = 0; i < count; i++) {
        free(procs[i].references);
    }

    return 0;
}
