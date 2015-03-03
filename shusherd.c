#include <signal.h>
#include <stdlib.h>

#include <unistd.h>
#include <sys/uio.h>
#include <sys/types.h>

#include <math.h>

#include <assert.h>
#include <fcntl.h>

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <sys/select.h>

#include <libdaemon/dfork.h>
#include <libdaemon/dsignal.h>
#include <libdaemon/dlog.h>
#include <libdaemon/dpid.h>
#include <libdaemon/dexec.h>

#include <libconfig.h>

#include <pthread.h>

#include <ebur128.h>

#include <sndfile.h>

#include <pulse/simple.h>
#include <pulse/error.h>

#define BUFSIZE 1024


#define DEFAULT_CONFIG "shusherrc"
#define DEFAULT_DECAY 0.20
#define DEFAULT_THRESHOLD 40
#define DEFAULT_INPUT_FILENAME "system:capture_1"
#define DEFAULT_SHUSHFILE "blah.wav"
#define DEFAULT_VERBOSITY LOG_DEBUG /* This is BROKEN for anything other than LOG_DEBUG! */

#define DEFAULT_RB_SIZE 16384

#define SAMPLE_TIME 3


typedef struct {
  int verbosity;
  int points_threshold;
  double decay;
  const char *shush_filename;
  const char *input_filename;
  SNDFILE *sndfile;
  SF_INFO snd_fileinfo;
  config_t config;
  ebur128_state *ebur128_state;
  pthread_t audio_thread;
  int can_capture;
  int can_process;
  int enable_processing;
  pthread_mutex_t lock;
  pthread_cond_t data_ready;
  pa_simple *pa;
} context_t;

void audio_trigger(context_t *context) {
  daemon_log(LOG_INFO, "Trigger %s", context->shush_filename);

  static const pa_sample_spec ss = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 1
  };

  pa_simple *s = NULL;
  int ret = 1;
  int error;
  int input_fd = open(context->shush_filename, O_RDONLY);
  if (input_fd < 0) {
    fprintf(stderr, "Error reading %s: %s\n", context->shush_filename, strerror(errno));
    return;
  }
  if (!(s = pa_simple_new(NULL, "shusherd", PA_STREAM_PLAYBACK, NULL, "playback", &ss, NULL, NULL, &error))) {
        fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n", pa_strerror(error));
        goto finish;
    }
    for (;;) {
        uint8_t buf[BUFSIZE];
        ssize_t r;
        if ((r = read(input_fd, buf, sizeof(buf))) <= 0) {
            if (r == 0) /* EOF */
                break;
            fprintf(stderr, __FILE__": read() failed: %s\n", strerror(errno));
            goto finish;
        }
        /* ... and play it */
        if (pa_simple_write(s, buf, (size_t) r, &error) < 0) {
            fprintf(stderr, __FILE__": pa_simple_write() failed: %s\n", pa_strerror(error));
            goto finish;
        }
    }
    /* Make sure that every single sample was played */
    if (pa_simple_drain(s, &error) < 0) {
        fprintf(stderr, __FILE__": pa_simple_drain() failed: %s\n", pa_strerror(error));
        goto finish;
    }
    ret = 0;
finish:
  close(input_fd);
    if (s)
        pa_simple_free(s);
}

void *audio_loop(void *context_p) {
  context_t *context = (context_t *)context_p;
  double *buffer;
  time_t t = time(NULL);
  double loudness;
  double points = 0.0;
  int error;

  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

  daemon_log(LOG_INFO, "Starting listening...");

  const short buf[BUFSIZE/2];

  int bytes = 0;
  int trigger = 0;

  while (context->enable_processing) {
    if ((bytes = pa_simple_read(context->pa, (void *)buf, sizeof(buf), &error)) < 0) {
      daemon_log(LOG_ERR, "pa_simple_read failed: %s", pa_strerror(error));
      assert(0);
    }

  ebur128_add_frames_short(context->ebur128_state, buf, sizeof(buf)/2);

  if ((time(NULL) - t) > SAMPLE_TIME) {
    t = time(NULL);
    ebur128_loudness_shortterm(context->ebur128_state, &loudness);

    points += 100 - fabs(loudness);

    daemon_log(LOG_INFO, "Points: %f (%d) (%f)", points, context->points_threshold, loudness);

    if (points > context->points_threshold) {
      trigger = 1;
    } else {
      points *= context->decay;
    }
  }

  if (trigger) {
    context->can_process = 0;
    audio_trigger(context);
    points = 0;
    trigger = 0;
    context->can_process = 1;
  }
  }

  daemon_log(LOG_INFO, "Stopped listening...");


 // free(buffer);
  //free(framebuf);
  return 0;
}

int audio_init(context_t *context) {
  int rc = 0;
  static const pa_sample_spec ss = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 2
  };
  int error;

  memset(&context->snd_fileinfo, '\0', sizeof(context->snd_fileinfo));
  daemon_log(LOG_INFO, "Opening %s", context->input_filename);
  //int fd = open(context->input_filename, O_RDONLY);
  //context->sndfile = sf_open_fd(fd, SFM_READ, &context->snd_fileinfo, 1);

  context->pa = pa_simple_new(NULL, "shusherd", PA_STREAM_RECORD, context->input_filename, "record", &ss, NULL, NULL, &error);
  if (!context->pa) {
    daemon_log(LOG_ERR, "pa_simple_new failed: %s", pa_strerror(error));
    assert(0);
  }
  context->snd_fileinfo.samplerate = 44100;
  context->snd_fileinfo.channels = 1;
  //context->snd_fileinfo.samplerate = jack_get_sample_rate(context->client);
  //context->snd_fileinfo.channels = 1;

  context->ebur128_state = ebur128_init((unsigned)context->snd_fileinfo.channels,
                                        (unsigned)context->snd_fileinfo.samplerate,
                                        EBUR128_MODE_S);
  if (!context->ebur128_state) {
    daemon_log(LOG_ERR, "Unable to initialize ebur128");
    goto cleanup_sndfile;
  }


  //memset(context->in, 0, sizeof(*context->in));
  //memset(context->rb->buf, 0, context->rb->size);

  char name[64];
  sprintf(name, "input%d", 1);

  //assert(rc == 0);
  context->can_process = 1;
  context->can_capture = 1;
  context->enable_processing = 1;

  audio_loop(context);
  //rc = pthread_create(&context->audio_thread, NULL, audio_loop, (void *)context);
  if (rc) {
    daemon_log(LOG_ERR, "Unable to create audio thread: %d", rc);
    goto cleanup_ebur128_state;
  }

  return 0;

cleanup_ebur128_state:
  ebur128_destroy(&context->ebur128_state);
cleanup_sndfile:
  //sf_close(context->sndfile);
  return rc;
}

void audio_destroy(context_t *context) {
  context->enable_processing = 0;
  pthread_join(context->audio_thread, NULL);
  ebur128_destroy(&context->ebur128_state);
  sf_close(context->sndfile);
  pthread_mutex_destroy(&context->lock);
  pthread_cond_destroy(&context->data_ready);
}

int settings_init(context_t *context) {
  int rc = config_read_file(&context->config, DEFAULT_CONFIG);

  if (rc == CONFIG_FALSE) {
    daemon_log(LOG_ERR, "config: %s:%d: %s",
                         config_error_file(&context->config) ?: "",
                         config_error_line(&context->config),
                         config_error_text(&context->config));
    return 1;
  }

  context->decay = DEFAULT_DECAY;
  context->points_threshold = DEFAULT_THRESHOLD;
  context->shush_filename = DEFAULT_SHUSHFILE;
  //context->input_filename = DEFAULT_INPUT_FILENAME;
  context->input_filename = NULL;
  context->verbosity = DEFAULT_VERBOSITY;

  config_lookup_float(&context->config, "decay", &context->decay);
  config_lookup_int(&context->config, "threshold", &context->points_threshold);
  config_lookup_string(&context->config, "shush_file", &context->shush_filename);
  config_lookup_string(&context->config, "input_file", &context->input_filename);
  config_lookup_bool(&context->config, "verbosity", &context->verbosity);

  daemon_set_verbosity(context->verbosity);

  daemon_log(LOG_DEBUG, "Settings:");
  daemon_log(LOG_DEBUG, "\t%.10s %.01f", "decay", context->decay);
  daemon_log(LOG_DEBUG, "\t%.10s %d", "threshold", context->points_threshold);
  daemon_log(LOG_DEBUG, "\t%.10s %s", "shush_file", context->shush_filename);
  daemon_log(LOG_DEBUG, "\t%.10s %s", "input_file", context->input_filename);
  daemon_log(LOG_DEBUG, "\t%.10s %d", "verbosity", context->verbosity);

  return 0;
}

void settings_destroy(context_t *context) {
  config_destroy(&context->config);
}

int main(int argc, char *argv[]) {
  int rc;
  context_t context = {0};
  int fd, quit = 0;
  fd_set fds;

  daemon_log_use = DAEMON_LOG_STDERR;
  if (daemon_reset_sigs(-1) < 0) {
    daemon_log(LOG_ERR, "Failed to reset all signal handlers: %s", strerror(errno));
    return 1;
  }

  if (daemon_unblock_sigs(-1) < 0) {
    daemon_log(LOG_ERR, "Failed to unblock all signals: %s", strerror(errno));
    return 1;
  }

  daemon_pid_file_ident = daemon_log_ident = daemon_ident_from_argv0(argv[0]);

    rc = settings_init(&context);
    if (rc) {
      goto finish;
    }

    rc = audio_init(&context);
    if (rc) {
      goto finish;
    }

    daemon_log(LOG_INFO, "Sucessfully started");

    if (daemon_signal_init(SIGINT, SIGTERM, SIGQUIT, SIGHUP, 0) < 0) {
      daemon_log(LOG_ERR, "Could not register signal handlers (%s).", strerror(errno));
      goto finish;
    }

    /* Prepare for select() on the signal fd */
    FD_ZERO(&fds);
    fd = daemon_signal_fd();
    FD_SET(fd, &fds);

    while (!quit) {
      fd_set fds2 = fds;

      /* Wait for an incoming signal */
      if (select(FD_SETSIZE, &fds2, 0, 0, 0) < 0) {

        /* If we've been interrupted by an incoming signal, continue */
        if (errno == EINTR)
          continue;

        daemon_log(LOG_ERR, "select(): %s", strerror(errno));
        break;
      }

      /* Check if a signal has been recieved */
      if (FD_ISSET(fd, &fds2)) {
        int sig;

        /* Get signal */
        if ((sig = daemon_signal_next()) <= 0) {
          daemon_log(LOG_ERR, "daemon_signal_next() failed: %s", strerror(errno));
          break;
        }

        /* Dispatch signal */
        switch (sig) {

          case SIGINT:
          case SIGQUIT:
          case SIGTERM:
            daemon_log(LOG_WARNING, "Got SIGINT, SIGQUIT or SIGTERM.");
            quit = 1;
            break;

        }
      }
    }

finish:
    audio_destroy(&context);
    settings_destroy(&context);
    daemon_log(LOG_INFO, "Exiting...");
    daemon_retval_send(255);
    daemon_signal_done();
    daemon_pid_file_remove();

    return 0;
}
