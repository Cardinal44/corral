# Baseline comparison

Machine: Ubuntu 26.04 LTS, kernel 7.0.0-31-generic, x86_64, 2 CPUs.

TO is `timeout (uutils coreutils) 0.8.0`.

Each cell: exit status of the runner (`hung` if it was killed after 7 s), the worst survivor count over the repetitions, and the median duration.

| ID | CE | CF | TO | NV |
|---|---|---|---|---|
| F1 | 124; 0 surv; 2.02 s | 124; 0 surv; 2.02 s | 124; 0 surv; 2.12 s | 137; 0 surv; 2.07 s |
| F2 | 0; 0 surv; 0.03 s | 0; 0 surv; 0.02 s | 124; 0 surv; 2.12 s | 137; 0 surv; 2.07 s |
| F3 | 0; 0 surv; 0.07 s | 0; 0 surv; 0.03 s | 0; 1 surv; 0.12 s | hung; 1 surv; 7.00 s |
| F4 | 0; 0 surv; 0.03 s | 0; 0 surv; 0.02 s | 0; 1 surv; 0.12 s | hung; 1 surv; 7.00 s |
| F5 | 124; 0 surv; 2.02 s | 124; 0 surv; 2.02 s | 124; 1 surv; 2.12 s | hung; 1 surv; 7.00 s |
| F6 | 124; 0 surv; 3.03 s | 124; 0 surv; 3.03 s | 137; 0 surv; 3.03 s | 137; 0 surv; 2.07 s |
| F7 | 124; 0 surv; 2.02 s | 124; 0 surv; 2.08 s | 124; 1 surv; 2.12 s | hung; 192 surv; 7.00 s |
| F8 | 0; 0 surv; 0.03 s | 0; 0 surv; 0.02 s | 0; 1 surv; 0.12 s | hung; 1 surv; 7.00 s |
| F9 | 121; 0 surv; 0.12 s | 124; 0 surv; 2.07 s | 124; 0 surv; 2.12 s | 137; 0 surv; 2.12 s |
| F10 | 124; 0 surv; 2.02 s; cleanup output | 124; 0 surv; 2.02 s; cleanup output | 124; 0 surv; 2.12 s; cleanup output | 137; 0 surv; 2.07 s |

CLAIM HOLDS
