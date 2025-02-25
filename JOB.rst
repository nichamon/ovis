=================
Job Info in LDMSD
=================

Slurm
=====

Currently capturing information about:

- jobs:

  - job_id (int)
  - app_id (int)
  - user (char*)
  - job_name (char*)
  - job_tag (char*)
  - job_state
  - job_size
  - job_uid
  - job_gid
  - job_start
  - job_end
  - node_count
  - task_count

- tasks

  - job_id
  - task_pid
  - task_rank
  - task_exit_status

NOTE:
- In ``slurm_sampler.c`` the multitenancy is captured in array, e.g.
  ``job_id`` is ``u64_array``.
- In ``slurm_sampler2.c`` the multitenancy is captured in records. Each job has
  its own ``job_rec`` (and so does task with ``task_rec``).

Jobs and Tasks in ``slurm`` is flat and not complicated. The ``slurm_notifier``
Plug Stack plugin informs ``slurm_sampler`` (and ``slurm_sampler2``) about Jobs
and Tasks events.


Flux
====

Simple Batch Job
----------------

A simple batch job with just ``flux run`` in the job script (no recursive
``flux batch``).

``jobs/simple.sh``:

.. code-block:: bash

   #!/bin/bash
   # flux: -N2
   # flux: -t 2m

   flux run -N2 ${PWD}/app1.py


Job submission:

.. code-block:: bash

   $ flux batch jobs/simple.sh


Process tree:

.. code-block:: bash

   # flux-1
   USER    PID CMD
   flux   1262 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   2880  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3371397661327360
   narate 2884      \_ /usr/local/libexec/flux/flux-shell 3371397661327360
   narate 2885          \_ broker /tmp/jobtmp-0-ƒTKuuBXJyV/script
   narate 2938              \_ /bin/bash /tmp/jobtmp-0-ƒTKuuBXJyV/script
   narate 2939              |   \_ flux-job attach ƒD7j8aT
   narate 2940              \_ /usr/bin/python3 /usr/local/libexec/flux/cmd/py-runner.py /usr/local/libexec/flux/cmd/flux-job-validator.py
   narate 2942              \_ /usr/local/libexec/flux/flux-shell 7952400384
   narate 2943                  \_ python3 /home/narate/projects/flux-simple-shell-plugin/app1.py

   # flux-2
   USER    PID CMD
   flux    934 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   2111  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3371397661327360
   narate 2115      \_ /usr/local/libexec/flux/flux-shell 3371397661327360
   narate 2116          \_ broker /tmp/jobtmp-1-ƒTKuuBXJyV/script
   narate 2144              \_ /usr/local/libexec/flux/flux-shell 7952400384
   narate 2145                  \_ python3 /home/narate/projects/flux-simple-shell-plugin/app1.py

Notice that from a simple ``flux batch`` even without nested job submission,
``flux`` creates nested instances (brokers).

Observations:

* ``flux_shell_get_info()`` from ``shell.init`` reports the following Job ID:

  * on flux-1

    * [flux-shell pid:2884] 3371397661327360 ('ƒTKuuBXJyV')

      * [flux-shell pid:2942] 7952400384 ('ƒD7j8aT')

  * on flux-2

    * [flux-shell pid:2115] 3371397661327360 ('ƒTKuuBXJyV')

      * [flux-shell pid:2144] 7952400384 ('ƒD7j8aT')

* ``FLUX_JOB_ID`` env var from ``app1.py``:

  * on flux-1

    * "FLUX_JOB_ID": "\u0192D7j8aT" ('ƒD7j8aT')

  * on flux-2

    * "FLUX_JOB_ID": "\u0192D7j8aT" ('ƒD7j8aT')

The application does not see the main Job ID.


Nested job
----------

The main job script.

.. code-block:: bash

   #!/bin/bash
   # flux: -N4

   flux batch jobs/app1.sh
   flux batch jobs/app2.sh
   flux queue drain

The sub-job scripts (for app1)

.. code-block:: bash

   #!/bin/bash
   # flux: -N1

   flux run -N1 -n3 ${PWD}/app1.py

The sub-job scripts (for app2)

.. code-block:: bash

   #!/bin/bash
   # flux: -N3

   flux run -N3 -n6 ${PWD}/app2.py


Process tree:

.. code-block:: bash

   # flux-1
   USER    PID CMD
   flux   1262 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   2660  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 2310925306560512
   narate 2664      \_ /usr/local/libexec/flux/flux-shell 2310925306560512
   narate 2665          \_ broker /tmp/jobtmp-0-ƒK3d6Ax2oh/script
   narate 2718              \_ /bin/bash /tmp/jobtmp-0-ƒK3d6Ax2oh/script
   narate 2749              |   \_ /usr/bin/python3 /usr/local/libexec/flux/cmd/py-runner.py /usr/local/libexec/flux/cmd/flux-queue.py drain
   narate 2720              \_ /usr/bin/python3 /usr/local/libexec/flux/cmd/py-runner.py /usr/local/libexec/flux/cmd/flux-job-validator.py
   narate 2722              \_ /usr/local/libexec/flux/flux-shell 7751073792
   narate 2724                  \_ broker /tmp/flux-jDMebU/jobtmp-0-ƒCowHCK/script
   narate 2776                      \_ /bin/bash /tmp/flux-jDMebU/jobtmp-0-ƒCowHCK/script
   narate 2777                      |   \_ flux-job attach ƒ7oMhxj
   narate 2778                      \_ /usr/bin/python3 /usr/local/libexec/flux/cmd/py-runner.py /usr/local/libexec/flux/cmd/flux-job-validator.py
   narate 2780                      \_ /usr/local/libexec/flux/flux-shell 4462739456
   narate 2781                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app1.py
   narate 2782                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app1.py
   narate 2783                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app1.py


   # flux-2
   USER    PID CMD
   flux    934 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   1966  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 2310925306560512
   narate 1970      \_ /usr/local/libexec/flux/flux-shell 2310925306560512
   narate 1971          \_ broker /tmp/jobtmp-1-ƒK3d6Ax2oh/script
   narate 1999              \_ /usr/local/libexec/flux/flux-shell 9110028288
   narate 2000                  \_ broker /tmp/flux-T3vlnY/jobtmp-0-ƒEt2H2F/script
   narate 2053                      \_ /bin/bash /tmp/flux-T3vlnY/jobtmp-0-ƒEt2H2F/script
   narate 2054                      |   \_ flux-job attach ƒCi1L4w
   narate 2055                      \_ /usr/bin/python3 /usr/local/libexec/flux/cmd/py-runner.py /usr/local/libexec/flux/cmd/flux-job-validator.py
   narate 2057                      \_ /usr/local/libexec/flux/flux-shell 7683964928
   narate 2058                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app2.py
   narate 2059                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app2.py

   # flux-3
   USER    PID CMD
   flux    559 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   1269  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 2310925306560512
   narate 1273      \_ /usr/local/libexec/flux/flux-shell 2310925306560512
   narate 1274          \_ broker /tmp/jobtmp-2-ƒK3d6Ax2oh/script
   narate 1302              \_ /usr/local/libexec/flux/flux-shell 9110028288
   narate 1303                  \_ broker /tmp/flux-HOyl1H/jobtmp-1-ƒEt2H2F/script
   narate 1331                      \_ /usr/local/libexec/flux/flux-shell 7683964928
   narate 1332                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app2.py
   narate 1333                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app2.py

   # flux-4
   USER    PID CMD
   flux    553 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   1263  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 2310925306560512
   narate 1267      \_ /usr/local/libexec/flux/flux-shell 2310925306560512
   narate 1268          \_ broker /tmp/jobtmp-3-ƒK3d6Ax2oh/script
   narate 1296              \_ /usr/local/libexec/flux/flux-shell 9110028288
   narate 1297                  \_ broker /tmp/flux-7Y8ADG/jobtmp-2-ƒEt2H2F/script
   narate 1325                      \_ /usr/local/libexec/flux/flux-shell 7683964928
   narate 1326                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app2.py
   narate 1327                          \_ python3 /home/narate/projects/flux-simple-shell-plugin/app2.py


Observations:
* ``flux_shell_get_info()`` from ``shell.init`` reports the following Job ID:

  * on flux-1

    * [flux-shell pid:2664] 2310925306560512 ('ƒK3d6Ax2oh')

      * [flux-shell pid:2722] 7751073792 ('ƒCowHCK')

        * [flux-shell pid:2780] 4462739456 ('ƒ7oMhxj')

  * on flux-2

    * [flux-shell pid:1966] 2310925306560512 ('ƒK3d6Ax2oh')

      * [flux-shell pid:1999] 9110028288 ('ƒEt2H2F')

        * [flux-shell pid:2057] 7683964928 ('ƒCi1L4w')

  * on flux-3

    * [flux-shell pid:1273] 2310925306560512 ('ƒK3d6Ax2oh')

      * [flux-shell pid:1302] 9110028288 ('ƒEt2H2F')

        * [flux-shell pid:1331] 7683964928 ('ƒCi1L4w')

  * on flux-4

    * [flux-shell pid:1267] 2310925306560512 ('ƒK3d6Ax2oh')

      * [flux-shell pid:1296] 9110028288 ('ƒEt2H2F')

        * [flux-shell pid:1325] 7683964928 ('ƒCi1L4w')

* ``app-1.py`` got the following:

  * on flux-1

    * env-var "FLUX_JOB_ID": "\u01927oMhxj" ('ƒ7oMhxj')

* ``app-2.py`` got the following

  * on flux-2

    * env-var "FLUX_JOB_ID": "\u0192Ci1L4w" ('ƒCi1L4w')

  * on flux-3

    * env-var "FLUX_JOB_ID": "\u0192Ci1L4w" ('ƒCi1L4w')

  * on flux-4

    * env-var "FLUX_JOB_ID": "\u0192Ci1L4w" ('ƒCi1L4w')

Notice that the ``FLUX_JOB_ID`` seen by the application is that from the
inner-most Flux instance.


Flux run w/o batch job submission
---------------------------------

Issue the interactive run as follows:

.. code-block:: bash

   flux run -N2 ./app1.py

Process tree:

.. code-block:: bash

   # on flux-1
   USER    PID CMD
   flux   1262 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   3000  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3579873981366272
   narate 3004      \_ /usr/local/libexec/flux/flux-shell 3579873981366272
   narate 3005          \_ python3 ./app1.py

   # on flux-2
   USER         PID CMD
   flux    934 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   2171  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3579873981366272
   narate 2175      \_ /usr/local/libexec/flux/flux-shell 3579873981366272
   narate 2176          \_ python3 ./app1.py
   narate 2177              \_ /bin/sh -c ps -wwaxfo user,pid,cmd
   narate 2178                  \_ ps -wwaxfo user,pid,cmd

Observations:

* ``flux_shell_get_info()`` from ``shell.init`` reports the following Job ID:

  * on flux-1

    * [flux-shell pid:3004] 3579873981366272 ('ƒUxLDi2ndD')

  * on flux-2

    * [flux-shell pid:2175] 3579873981366272 ('ƒUxLDi2ndD')

* ``FLUX_JOB_ID`` env var from ``app1.py``:

  * on flux-1

    * "FLUX_JOB_ID": "\u0192UxLDi2ndD" ('ƒUxLDi2ndD')

  * on flux-2

    * "FLUX_JOB_ID": "\u0192UxLDi2ndD" ('ƒUxLDi2ndD')

The Job ID that the application see in this case is from the system instance.


Multi-tenant simple jobs
------------------------

In this case we try submitting multiple jobs in multi-tenant configuration, with
``match-policy = "lonode"`` and ``node_exclusive = false`` in ``system.toml``.

``jobs/mt.sh`` job script:

.. code-block:: bash

   #!/bin/bash
   # flux: -N4
   # flux: -n4

   flux run -N4 -n4 ./app1.py

Submitting the job 4 times:

.. code-block:: bash

   $ flux batch jobs/mt.sh &
     flux batch jobs/mt.sh &
     flux batch jobs/mt.sh &
     flux batch jobs/mt.sh


Process Tree

.. code-block:: bash

   # on flux-1
   USER    PID CMD
   flux   3557 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   4185  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031643627520
   narate 4201  |   \_ /usr/local/libexec/flux/flux-shell 3705031643627520
   narate 4205  |       \_ broker /tmp/jobtmp-0-ƒVw1u84dRR/script
   narate 4422  |           \_ /bin/bash /tmp/jobtmp-0-ƒVw1u84dRR/script
   narate 4424  |           |   \_ flux-job attach ƒDokoBm
   narate 4437  |           \_ /usr/local/libexec/flux/flux-shell 8405385216
   narate 4440  |               \_ python3 ./app1.py
   narate 4447  |                   \_ /bin/sh -c ps -wwaxfo user,pid,cmd
   narate 4448  |                       \_ ps -wwaxfo user,pid,cmd
   root   4186  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031643627521
   narate 4203  |   \_ /usr/local/libexec/flux/flux-shell 3705031643627521
   narate 4206  |       \_ broker /tmp/jobtmp-0-ƒVw1u84dRS/script
   narate 4417  |           \_ /bin/bash /tmp/jobtmp-0-ƒVw1u84dRS/script
   narate 4418  |           |   \_ flux-job attach ƒDDf5hq
   narate 4433  |           \_ /usr/local/libexec/flux/flux-shell 8019509248
   narate 4436  |               \_ python3 ./app1.py
   root   4187  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031660404736
   narate 4204  |   \_ /usr/local/libexec/flux/flux-shell 3705031660404736
   narate 4208  |       \_ broker /tmp/jobtmp-0-ƒVw1u9Ychm/script
   narate 4421  |           \_ /bin/bash /tmp/jobtmp-0-ƒVw1u9Ychm/script
   narate 4423  |           |   \_ flux-job attach ƒDokoBm
   narate 4434  |           \_ /usr/local/libexec/flux/flux-shell 8405385216
   narate 4439  |               \_ python3 ./app1.py
   root   4190  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031660404737
   narate 4202      \_ /usr/local/libexec/flux/flux-shell 3705031660404737
   narate 4207          \_ broker /tmp/jobtmp-0-ƒVw1u9Ychn/script
   narate 4419              \_ /bin/bash /tmp/jobtmp-0-ƒVw1u9Ychn/script
   narate 4420              |   \_ flux-job attach ƒDVxwod
   narate 4435              \_ /usr/local/libexec/flux/flux-shell 8204058624
   narate 4438                  \_ python3 ./app1.py

   # on flux-2
   USER    PID CMD
   flux   2544 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   2884  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031643627520
   narate 2900  |   \_ /usr/local/libexec/flux/flux-shell 3705031643627520
   narate 2904  |       \_ broker /tmp/jobtmp-1-ƒVw1u84dRR/script
   narate 3020  |           \_ /usr/local/libexec/flux/flux-shell 8405385216
   narate 3023  |               \_ python3 ./app1.py
   narate 3030  |                   \_ /bin/sh -c ps -wwaxfo user,pid,cmd
   narate 3031  |                       \_ ps -wwaxfo user,pid,cmd
   root   2885  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031643627521
   narate 2901  |   \_ /usr/local/libexec/flux/flux-shell 3705031643627521
   narate 2905  |       \_ broker /tmp/jobtmp-1-ƒVw1u84dRS/script
   narate 3016  |           \_ /usr/local/libexec/flux/flux-shell 8019509248
   narate 3018  |               \_ python3 ./app1.py
   root   2886  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031660404736
   narate 2902  |   \_ /usr/local/libexec/flux/flux-shell 3705031660404736
   narate 2907  |       \_ broker /tmp/jobtmp-1-ƒVw1u9Ychm/script
   narate 3017  |           \_ /usr/local/libexec/flux/flux-shell 8405385216
   narate 3021  |               \_ python3 ./app1.py
   root   2888  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031660404737
   narate 2903      \_ /usr/local/libexec/flux/flux-shell 3705031660404737
   narate 2906          \_ broker /tmp/jobtmp-1-ƒVw1u9Ychn/script
   narate 3019              \_ /usr/local/libexec/flux/flux-shell 8204058624
   narate 3022                  \_ python3 ./app1.py

   # on flux-3
   USER    PID CMD
   flux   1733 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   2073  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031643627520
   narate 2089  |   \_ /usr/local/libexec/flux/flux-shell 3705031643627520
   narate 2093  |       \_ broker /tmp/jobtmp-2-ƒVw1u84dRR/script
   narate 2209  |           \_ /usr/local/libexec/flux/flux-shell 8405385216
   narate 2212  |               \_ python3 ./app1.py
   narate 2219  |                   \_ /bin/sh -c ps -wwaxfo user,pid,cmd
   narate 2220  |                       \_ ps -wwaxfo user,pid,cmd
   root   2074  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031643627521
   narate 2091  |   \_ /usr/local/libexec/flux/flux-shell 3705031643627521
   narate 2094  |       \_ broker /tmp/jobtmp-2-ƒVw1u84dRS/script
   narate 2205  |           \_ /usr/local/libexec/flux/flux-shell 8019509248
   narate 2206  |               \_ python3 ./app1.py
   root   2075  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031660404736
   narate 2092  |   \_ /usr/local/libexec/flux/flux-shell 3705031660404736
   narate 2095  |       \_ broker /tmp/jobtmp-2-ƒVw1u9Ychm/script
   narate 2207  |           \_ /usr/local/libexec/flux/flux-shell 8405385216
   narate 2210  |               \_ python3 ./app1.py
   root   2076  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031660404737
   narate 2090      \_ /usr/local/libexec/flux/flux-shell 3705031660404737
   narate 2096          \_ broker /tmp/jobtmp-2-ƒVw1u9Ychn/script
   narate 2208              \_ /usr/local/libexec/flux/flux-shell 8204058624
   narate 2211                  \_ python3 ./app1.py

   # on flux-4
   USER         PID CMD
   flux   1727 broker --config-path=/usr/local/etc/flux/system/conf.d ...
   root   2067  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031643627520
   narate 2084  |   \_ /usr/local/libexec/flux/flux-shell 3705031643627520
   narate 2087  |       \_ broker /tmp/jobtmp-3-ƒVw1u84dRR/script
   narate 2203  |           \_ /usr/local/libexec/flux/flux-shell 8405385216
   narate 2206  |               \_ python3 ./app1.py
   narate 2213  |                   \_ /bin/sh -c ps -wwaxfo user,pid,cmd
   narate 2214  |                       \_ ps -wwaxfo user,pid,cmd
   root   2068  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031643627521
   narate 2085  |   \_ /usr/local/libexec/flux/flux-shell 3705031643627521
   narate 2089  |       \_ broker /tmp/jobtmp-3-ƒVw1u84dRS/script
   narate 2199  |           \_ /usr/local/libexec/flux/flux-shell 8019509248
   narate 2200  |               \_ python3 ./app1.py
   root   2069  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031660404736
   narate 2083  |   \_ /usr/local/libexec/flux/flux-shell 3705031660404736
   narate 2090  |       \_ broker /tmp/jobtmp-3-ƒVw1u9Ychm/script
   narate 2201  |           \_ /usr/local/libexec/flux/flux-shell 8405385216
   narate 2204  |               \_ python3 ./app1.py
   root   2071  \_ /usr/local/libexec/flux/flux-imp exec /usr/local/libexec/flux/flux-shell 3705031660404737
   narate 2086      \_ /usr/local/libexec/flux/flux-shell 3705031660404737
   narate 2088          \_ broker /tmp/jobtmp-3-ƒVw1u9Ychn/script
   narate 2202              \_ /usr/local/libexec/flux/flux-shell 8204058624
   narate 2205                  \_ python3 ./app1.py

Observations:
* ``flux_shell_get_info()`` from ``shell.init`` reports the following Job ID:

  * on flux-1

    * [flux-shell pid:4201] 3705031643627520 ('ƒVw1u84dRR')

      * [flux-shell pid:4437] 8405385216 ('ƒDokoBm')

    * [flux-shell pid:4203] 3705031643627521 ('ƒVw1u84dRS')

      * [flux-shell pid:4433] 8019509248 ('ƒDDf5hq')

    * [flux-shell pid:4204] 3705031660404736 ('ƒVw1u9Ychm')

      * [flux-shell pid:4434] 8405385216 ('ƒDokoBm')

    * [flux-shell pid:4202] 3705031660404737 ('ƒVw1u9Ychn')

      * [flux-shell pid:4435] 8204058624 ('ƒDVxwod')

  * on flux-2

    * [flux-shell pid:2900] 3705031643627520 ('ƒVw1u84dRR')

      * [flux-shell pid:3020] 8405385216 ('ƒDokoBm')

    * [flux-shell pid:2901] 3705031643627521 ('ƒVw1u84dRS')

      * [flux-shell pid:3016] 8019509248 ('ƒDDf5hq')

    * [flux-shell pid:2902] 3705031660404736 ('ƒVw1u9Ychm')

      * [flux-shell pid:3017] 8405385216 ('ƒDokoBm')

    * [flux-shell pid:2903] 3705031660404737 ('ƒVw1u9Ychn')

      * [flux-shell pid:3019] 8204058624 ('ƒDVxwod')

  * on flux-3

    * [flux-shell pid:2089] 3705031643627520 ('ƒVw1u84dRR')

      * [flux-shell pid:2209] 8405385216 ('ƒDokoBm')

    * [flux-shell pid:2091] 3705031643627521 ('ƒVw1u84dRS')

      * [flux-shell pid:2205] 8019509248 ('ƒDDf5hq')

    * [flux-shell pid:2092] 3705031660404736 ('ƒVw1u9Ychm')

      * [flux-shell pid:2207] 8405385216 ('ƒDokoBm')

    * [flux-shell pid:2090] 3705031660404737 ('ƒVw1u9Ychn')

      * [flux-shell pid:2208] 8204058624 ('ƒDVxwod')

  * on flux-4

    * [flux-shell pid:2084] 3705031643627520 ('ƒVw1u84dRR')

      * [flux-shell pid:2203] 8405385216 ('ƒDokoBm')

    * [flux-shell pid:2085] 3705031643627521 ('ƒVw1u84dRS')

      * [flux-shell pid:2199] 8019509248 ('ƒDDf5hq')

    * [flux-shell pid:2083] 3705031660404736 ('ƒVw1u9Ychm')

      * [flux-shell pid:2201] 8405385216 ('ƒDokoBm')

    * [flux-shell pid:2086] 3705031660404737 ('ƒVw1u9Ychn')

      * [flux-shell pid:2202] 8204058624 ('ƒDVxwod')

* ``FLUX_JOB_ID`` env var from ``app1.py``:

  * on flux-[1-4]; the first job ('ƒVw1u84dRR')

    * "FLUX_JOB_ID": "\u0192DokoBm" ('ƒDokoBm')

  * on flux-[1-4]: the second job ('ƒVw1u84dRS')

    * "FLUX_JOB_ID": "\u0192DDf5hq" ('ƒDDf5hq')

  * on flux-[1-4]: the third job ('ƒVw1u9Ychm')

    * "FLUX_JOB_ID": "\u0192DokoBm" ('ƒDokoBm')

  * on flux-[1-4]: the fourth job ('ƒVw1u9Ychn')

    * "FLUX_JOB_ID": "\u0192DVxwod" ('ƒDVxwod')

Notice that the subjob of the first job (ƒVw1u84dRR) and the subjob of the third
job (ƒVw1u9Ychm) have the same Job ID: ƒDokoBm. The ƒDokoBm is a f58 form of
an integer value 8405385216, or 0x1f5000000, which is constructed from TS=0x1f5
(501ms), GEN_ID=0, SEQ=0. We just got lucky that these two subjobs happened to
started at 501ms from the starting time of their instances.


Flux Job ID
-----------

Users see jobid in FLUID base58 (f58) format, e.g. ``ƒK3d6Ax2oh`` (the leading
Unicode character ``ƒ`` is a decorative prefix). The ID is actually
actually 64-bit integer (see
https://flux-framework.readthedocs.io/projects/flux-rfc/en/latest/spec_19.html).

* ``flux_jobid_t`` is ``uint64_t`` (from ``libjob/job.h``).

* The JSON obtained from ``flux_shell_get_info()`` also report ``"jobid"`` as an
  integer.

* Job ID is constructed using: ``(ts_msec<<24)|(gen_id<<10)|seq`` where
  ``ts_msec`` is the milliseconds since *epoch*.

  * In non-system instance, the epoch is the job start time (user instance start
    time?)

  * In system instance, the epoch is offset by the timestamp of the previously
    recorded largest Job ID.

  * see:

    * ``src/common/libutil/fluid.c``

      * ``fluid_init()``

      * ``fluid_generate()``

      * ``update_timestamp()``

    * ``src/modules/job-ingest/job-ingests.c``

      * ``mod_main()``

      * ``ingest_add_job()``

The Job ID is unique within a flux instance. However, according to the Job ID
construction method, in the case of multi-tenant ``flux submit``, the
**sub**-Job ID from two different jobs could be the same as seen in the
"Multi-tenant simple job" section above. This is OK in Flux world ... but LDMS
cannot rely solely on the reported Job ID to distinguish the jobs.


Capturing the job nestedness
----------------------------

How about we use ``char *`` to capture multi-level Job ID for Flux. For example,
the subjob 'ƒDokoBm' of the parent job 'ƒVw1u84dRR' would have Job ID:
'ƒVw1u84dRR/ƒDokoBm' in LDMS.

Or, maybe we do not care at all about subjobs and just report tasks under the
main Job ID.


OpenPBS
=======

This is just a quick peek into OpenPBS. We seem to be able to get Job / Task
information via ``hook`` mecahnism (Python Script). See
``src/hooks/cgroups/pbs_cgroups.PY`` for an example of a hook. Also see
https://2021.help.altair.com/2021.1.2/PBS%20Professional/PBSHooks2021.1.2.pdf.

OpenPBS seems to support sbumitting a job within a job (e.g. ``qsub job.sh``
and we have another ``qsub`` command inside ``job.sh``; see ``qsub.c:main()``).
However, there seem to be no tracking between job and subjobs other than the
Job Array type (Job Array job being the parent and array of jobs being subjobs).
(NOTE: This is just a quick peek .. I could be wrong).

OpenPBS Job ID is a string (see ``src/server/req_quejob.c:req_quejob()`` and
``src/server/req_quejob.c:generate_objid()``).


LDMSD Job Manager Plugin
========================

An LDMSD Job Manager Plugin is a plugin that provides information related to
jobs. The information shall be stored in ``ldms_set_t`` and will be referred to
as ``job_set``. The owner of the ``job_set`` should be the user running the job
(or ``root``) to prevent other user from reading the ``job_set`` content. Since
there may be multiple jobs from multiple users running concurrently on a compute
node, we may have multiple ``job_set``.

The ``job_set`` shall contain at least one record definitions: ``task_rec_def``
that describe Task record. It ``task_list`` to keep the records of tasks.

``job_set`` shall have at least the following metrics:

* ``job_id`` (``char_array``)

* ``user`` (``char_array``)

* ``job_name`` (``char_array``)

* ``job_uid`` (``u32``)

* ``job_gid`` (``u32``)

* ``job_start`` (``ts``)

* ``job_end`` (``ts``)

* ``node_count`` (``u32``)

* ``task_rec_def`` (``record_def``)

* ``task_list`` (``list`` of ``task_rec``)


``task_rec_def`` shall have at least the following metrics:

* ``job_id`` (``char_array``)

* ``task_pid`` (``u32``)

* ``task_rank`` (``u32``)

* ``task_start`` (``u32``)

* ``task_end`` (``u32``)

* ``task_exit_status`` (``u32``)

The changes to ``base_sampler``'s ``job_id`` is covered in the ``base_sampler``
section.

There may be multiple ``job_set``, each representing a job. The owner of the
``job_set`` shall be the ``user`` that run the job. The permission of the
``job_set`` is advisable to be ``0440``, but should be configurable.

The job may come and go quickly. This may be so quick that the amount of time
from the job start to job end is less than an update interval. As such,
``ldmsd`` shall keep the ``job_set`` for a while (configurable; but how much?)
even after the associated job has finished so that the consumers of the
``job_set`` (e.g.  aggregator & store) has a chance to process it. For the same
reason, ``task_rec`` shall also not be deleted from the list. They can be
modified (e.g. set the ``task_end`` time and ``task_exit_status`` when the task
has ended), but shall not be deleted.

``ldmsd`` shall load only allowed **one** Job Manager plugin.

The following is the routine of LDMSD Job Manager Plugin, please refer to the
code block following the routine for function and structure signatures:

1. Similar to other kinds of LDMSD plugins, ``ldmsd`` will call ``get_plugin()``
   function to load the plugin.

2. ``job_manger->base.config()`` is called to pass options specified by
   users to the plugin. Even though the user does not specify any option, this
   function is called anyway with empty ``avl``.

3. ``ldmsd`` calls ``job_manager->get_job_schema()`` to get the job schema. A
   sanity check will be performed on the schema (e.g. check if it has at least
   the required metrics).

4. ``job_manager->start()`` is called to let the ``job_manager``
   knows that it can start manipulating ``job_set`` now.


5. The plugin populates the ``job_set``.

   - Use ``ldmsd_job_set_new(job_id)`` to create a new ``job_set`` for ``job_id``.
   - Use ``ldmsd_job_set_find(job_id)`` to find the existing ``job_set``.
   - Use ``ldmsd_job_set_delete(job_set)`` to tell ``ldmsd`` that the
     ``job_set`` is no longer needed. (``ldmsd`` will delay the deletion so that
     the downstream consumers have a chance to process it).

6. To help other parts of ``ldmsd`` process Job information when it arrives, the
   plugin could let ``ldmsd`` knows about its Job events, e.g. a starting of a
   job, or a starting of a task, by calling ``ldmsd_job_event_post()`` after
   the job/task records have been updated.

   - ``LDMSD_JOB_START`` event shall be posted after the new ``job_set`` has
     been created and populated for the new job.

   - When a task has started and ``task_rec`` has been added into the
     ``job_set`` and ``task_rec[task_start]`` and ``task_rec[task_pid]``
     have been populated, ``LDMSD_JOB_TASK_START`` event shall be posted.

   - When a task has ended and ``task_rec[task_end]`` and
     ``task_rec[task_exit_status]`` have been populated, ``LDMSD_JOB_TASK_END``
     event shall be posted.

   - When the job exited and ``job_set[job_end]`` has been set,
     ``LDMSD_JOB_END`` event shall be posted.

   - ``LDMSD_JOB_SET_DELETE`` event is automatically posted by ``ldmsd`` after
     the job manager plugin calls ``ldmsd_job_set_delete(job_set)`` to let the
     clients know that ``job_set`` will no longer be available.

   The other part of ``ldmsd`` (e.g. a sampler plugin) may register to job
   events by calling ``ldmsd_job_event_subscribe()`` with a callback function,
   and call ``ldmsd_job_event_client_close()`` to stop the subscription.

7. ``job_manager->stop()`` is called to let ``job_manager`` know that
   it should stop manipulating ``job_set`` and stop posting job events.

8. ``job_manager->base.term()`` is called by ``ldmsd`` to terminate the plugin.


The following code block is a proposed API for managing ``job_set`` and
interaction with ``job_manager_plugin``.

.. code-block:: C

   struct ldmsd_job_manager {
     struct ldmsd_plugin base;

     /* Called by `ldmsd` to get the schema of a `job_set` */
     ldms_schema_t (*get_job_schema)(ldmsd_plugin_t self);

     /* Called by `ldmsd` to start the plugin.
      * The plugin can then start manipulating \c job_set. */
     void (*start)(ldmsd_plugin_t self);

     /* Called by `ldmsd` to stop the plugin.
      * The plugin shall not manipulate \c job_set after this function is called. */
     void (*stop)(ldmsd_plugin_t self);

   };

   /* These are functions for plugins to call to manage `job_set` with ldmsd. */

   /* Tell `ldmsd` to create a jobset.
    *
    * \retval job_set  A handle to the job set.
    * \retval NULL     If there is an error. \c errno will be set.
    */
   ldms_set_t ldmsd_job_set_new(const char *job_id);

   /* Find the \c job_set of the given \c job_id.
    *
    * \retval job_set  A handle to the job set.
    * \retval NULL     If the job set for the \c job_id is not found.
    *                  \c errno is set to \c ENOENT.
    */
   ldms_set_t ldmsd_job_set_find(const char *job_id);

   /* Tell \c ldmsd that the \c job_set is no longer needed by the plugin. */
   void ldmsd_job_set_delete(ldms_set_t job_set);

   enum ldmsd_job_event_type {
     LDMSD_JOB_START,
     LDMSD_JOB_TASK_START,
     LDMSD_JOB_TASK_END,
     LDMSD_JOB_END,
     LDMSD_JOB_SET_DELETE, /* the job_set is going away ... stop using it */
     LDMSD_JOB_CLIENT_CLOSE, /* the last event delivered to the cb fn */
   };

   struct ldmsd_job_event {
     enum ldmsd_job_event_type type;
     ldms_set_t  job_set;     /* for client side convenience */
     ldms_mval_t job_record;
     ldms_mval_t task_record; /* NULL if type is not LDMSD_JOB_TASK_{START|END} */
   };

   int ldmsd_job_event_post(const struct ldmsd_job_event *ev);

   void (*ldmsd_job_event_cb_fn_t)(ldmsd_job_event_client_t c,
                              const struct ldmsd_job_event *ev,
                              void *arg);

   ldmsd_job_event_client_t ldmsd_job_event_subscribe(
                                ldmsd_sampler_t samp /* can be NULL */,
                                ldmsd_job_event_cb_fn_t cb,
                                void *arg);
   void ldmsd_job_event_client_close(ldmsd_job_event_client_t c);

   /* get the first job */
   ldms_mval_t ldmsd_job_first();
   /* get the next job */
   ldms_mval_t ldmsd_job_next(ldms_mval_t job);
   /* get the first task in the list */
   ldms_mval_t ldmsd_task_first();
   /* get the next task job */
   ldms_mval_t ldmsd_task_next(ldms_mval_t task);



Use Case with Curated Metric Set
================================

A curated metric set is a metric set managed by ``curated_metric_sampler`` which
can be configured to collect pre-defined collection of metrics. There may be
multiple of curated metric sets.  For example, in multi-tenant environment, we
may have a curated set for each user whose job is running on the node.

Curated metrics may be categorized into three main categories: system-level
metrics, job-level metrics, and task-level metrics.

**System-level metrics** may exist as primitive metrics (e.g. ``MemFree``)  or
as records in a list (e.g. ``net_dev_rec`` similar to that in ``procnetdev2``
sampler).

**Job-level metrics** shall appear as records in a list. Each of the record will
have at least ``job_id`` as a member to refer to the job the record belongs to.
There may be multiple lists for different kinds of job-level records. Different
kinds of records (different definitions) shall not be in the same list.

**Task-level metrics** is similar to job-level metrics, but must contain
``job_id`` and ``task_id`` (``pid``?) in the records to refer to the tasks.
There may be multiple task-level metric list.

A curated metric set may have only system-level metrics, or only job-level and
task-level metrics for a certain user.

The curated metric set schema may look like a mix of the following:

* system-metric-1
* system-metric-2
* ...

* job_id_list: a list of job IDs so that the downstream can associate
  system-level metrics with these Jobs.

* job_level_rec_list1
* job_level_rec_list2
* ...
* task_level_rec_list1
* task_level_rec_list2
* ...

The curated metric sampler calls ``ldmsd_job_event_subscribe()`` to subscribe to
job events. The event is delivered from the same ``sampler`` thread if ``samp``
is supplied at the subscription, so no need to worry about the race between
events (and ``sample()`` calls). If ``samp`` was not given to
``ldmsd_job_event_subscribe()``, the job events will be delivered from one of
the ``ldmsd`` worker thread. All further events will be delivered by the same
thread.

The per-job/per-user curated metric sets may use ``LDMSD_JOB_SET_DELETE`` event,
which is triggered after the delayed deletion time of the ``job_set`` has
reached, to also delete the associated curated metric set after the delayed
deletion time has reached.


``base_sampler``
================

The set created by samplers extending ``base_sampler`` has ``job_id`` metric
which is sampled from the ``job_set`` (see ``base_sample_begin()``). The
``job_id`` in the previous implementation is ``u64``.

To support multi-tenancy in the sampler set, the ``job_id`` in the sampler set
is now ``char[]``, containing a string representing all currently running jobs.
The community asked to have a configurable separater that separates jobs in this
string.

Mechanism: TODO Think about this ...

What to do?

- Iterate through all ``job_set`` at ``base_sample_begin()``

- Has a global ``job_ids`` and copy from that at ``base_sample_begin()``

  - Using ref and mutex to manage value getting/setting.

- Modify the sampler set ``job_id`` based on job events.


Job Event Mechanism
===================

This section describes how job events are managed in ``ldmsd``.

Job Event Flow

1. Job Manager Plugin get ``job`` or ``task`` event from the Job Manager.

2. Job Manager Plugin update the job/task record accordingly.

3. Job Manager Plugin post the job/task event to ``ldmsd``

   .. code-block:: C

      struct ldmsd_job_event ev = {
        .type = LDMSD_JOB_TASK_START, /* or other event */
        .job_set = job_set,
        .task_record = task_rec, /* could be NULL */
      };
      ldmsd_job_event_post(&ev);

   The ``ev`` can be discarded after the call.

4. In ``ldmsd_job_event_post(ev)``, for each client registered for the job
   events:

   a) ``job_set`` reference is taken, to prevent it from being deleted.

   b) Create a new event ``_ev``, a copy of ``ev``, and post it to the target
      thread, with an interposer callback function.

   c) The interposer callback function calls the client callback. After the
      client callback function returned, the interposer function drop the
      ``job_set`` reference and free the ``_ev`` event.

By holding on to the ``job_set`` reference, we don't have to worry about the
deletion of the ``job_set`` before the event is delivered. In addition, the
``task_rec`` are append-only, and are only modified sch that the previously set
information (e.g. ``job_rec.task_start``) still persist. So, the client can
reliably use ``job_set`` and ``job_rec`` specified in the ``_ev``. However, this
does not guarantee that the delivered event is *CURRENT*. For example, if the
task is extremely short, by the time ``LDMSD_JOB_TASK_START`` is delivered, the
task may have alreay exited (no process in ``/proc``).

.. code-block:: ASCII

  Abbreviations:
  - job_mgr_plug - the Job Manager Plugin
  - cu_samp - the curated metric sampler
  - job_ev_post() - ldmsd_job_event_post()
  - ev_post() - the internal event posting
  - int_cb() - the internal interposer callback function
  - cli_cb() - the event callback function of the job event client

  .------------.          .-----.             .-------.
  |job_mgr_plug|          |ldmsd|             |cu_samp|
  '------------'          '-----'             '-------'
        |                    |                    |
        | job_ev_post(ev)    |                    |
        |------------------->|                    |
        |                    |    ev_post(_ev)    |
        |                    |------------------>.-.
        |                    |                   | | int_cb(_ev)
        |                    |                   | |
        |                    |                   |.-.
        |                    |                   || | cli_cb(_ev)
        |                    |                   || |
        |                    |                   || |
        |                    |                   |'-'
        |                    |                   | |
        |                    |                   | |
        |                    |                   '-'
        |                    |                    |
        |                    |                    |
        v                    v                    v



On Storage Side
===============

On the aggregator, the ``job_set`` appears as a regular ``ldms_set``. It can be
fed to legacy store path or decomposition path like other sets. The delayed
deletion with enough time allows ``job_set`` to be stored even if the job is a
short job. The ``data_gn`` will prevent redundant storage of the ``job_set``.

Similar to the ``job_set``, the curated metric set can use the same delayed
delete tactic (on ``LDMSD_JOB_SET_DELETE`` event) to ensure that the set would
have enough time to be processed in the aggregator.
