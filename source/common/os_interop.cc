/* Windows/Linux interoperability layer
 *
 * Copyright (C) 2020-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@rhizomatica.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "common/os_interop.h"

#if defined(_WIN32)

int get_temp_path(char* pathBuffer, int pathBufferSize, const char* pathPart)
{
	const char* temp = getenv(TMP_ENV_NAME);
	if(strlen(temp) >= pathBufferSize - strlen(pathPart+1)) {
		return 0;
	}

	/* We've done the size check above so we don't need to use the string safe methods */
	strcpy(pathBuffer, temp);
    strcat(pathBuffer, "\\");
	strcat(pathBuffer, pathPart+1);
	return 1;
}

int MUTEX_LOCK(HANDLE *mqh_lock)
{
	DWORD dwWaitResult = WaitForSingleObject(
		mqh_lock,    // handle to mutex
		-1);  // no time-out interval

    switch (dwWaitResult)
    {
        // The thread got ownership of the mutex
        case WAIT_OBJECT_0:
			return 0;

        // The thread got ownership of an abandoned mutex
        // The database is in an indeterminate state
        case WAIT_ABANDONED:
	  return ECANCELED ;
    default:
		  return EINVAL;
    }
}

/* Returns 0 on success */
int COND_WAIT(HANDLE *mqh_wait, HANDLE *mqh_lock)
{
	DWORD dwWaitResult;

	ReleaseMutex(mqh_lock);
	dwWaitResult = WaitForSingleObject(mqh_wait, -1);
	if(dwWaitResult != WAIT_OBJECT_0) {
		return EINVAL;
	}

	return MUTEX_LOCK(mqh_lock);
}

/* Returns 0 on success */
int COND_TIMED_WAIT(HANDLE *mqh_wait, HANDLE *mqh_lock, const struct timespec* abstime)
{
	DWORD dwWaitResult;

	ReleaseMutex(mqh_lock);
	dwWaitResult = WaitForSingleObject(mqh_wait, (DWORD) abstime->tv_sec);
	switch(dwWaitResult) {
	    case WAIT_OBJECT_0:
	        break;
	    case WAIT_TIMEOUT:
	        return ETIMEDOUT;
	    default:
	        return EINVAL;
	}

	dwWaitResult = WaitForSingleObject(mqh_lock,(DWORD) abstime->tv_sec);
	if(dwWaitResult == WAIT_OBJECT_0) {
	    return 0;
	}

    if(dwWaitResult == WAIT_TIMEOUT) {
        return ETIMEDOUT;
    }

    return EINVAL;
}

/* Returns 0 on success */
int COND_SIGNAL(HANDLE *mqh_wait)
{
	BOOL result = SetEvent(mqh_wait);
	return result == 0;
}
void MUTEX_UNLOCK(HANDLE *mqh_lock)
{
	ReleaseMutex(mqh_lock);
}

#endif

// glibc 2.39 rand without locks
// removed comments for clarity
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

/* Linear congruential.  */
#define	TYPE_0		0
#define	BREAK_0		8
#define	DEG_0		0
#define	SEP_0		0

/* x**7 + x**3 + 1.  */
#define	TYPE_1		1
#define	BREAK_1		32
#define	DEG_1		7
#define	SEP_1		3

/* x**15 + x + 1.  */
#define	TYPE_2		2
#define	BREAK_2		64
#define	DEG_2		15
#define	SEP_2		1

/* x**31 + x**3 + 1.  */
#define	TYPE_3		3
#define	BREAK_3		128
#define	DEG_3		31
#define	SEP_3		3

/* x**63 + x + 1.  */
#define	TYPE_4		4
#define	BREAK_4		256
#define	DEG_4		63
#define	SEP_4		1


#define	MAX_TYPES	5	/* Max number of types above.  */

struct random_poly_info
{
  int seps[MAX_TYPES];
  int degrees[MAX_TYPES];
};

static const struct random_poly_info random_poly_info =
{
  { SEP_0, SEP_1, SEP_2, SEP_3, SEP_4 },
  { DEG_0, DEG_1, DEG_2, DEG_3, DEG_4 }
};

static int32_t randtbl[DEG_3 + 1] =
  {
    TYPE_3,

    -1726662223, 379960547, 1735697613, 1040273694, 1313901226,
    1627687941, -179304937, -2073333483, 1780058412, -1989503057,
    -615974602, 344556628, 939512070, -1249116260, 1507946756,
    -812545463, 154635395, 1388815473, -1926676823, 525320961,
    -1009028674, 968117788, -123449607, 1284210865, 435012392,
    -2017506339, -911064859, -370259173, 1132637927, 1398500161,
    -205601318,
  };

struct random_data_t
  {
    int32_t *fptr;		/* Front pointer.  */
    int32_t *rptr;		/* Rear pointer.  */
    int32_t *state;		/* Array of state values.  */
    int rand_type;		/* Type of random number generator.  */
    int rand_deg;		/* Degree of random number generator.  */
    int rand_sep;		/* Distance between front and rear.  */
    int32_t *end_ptr;		/* Pointer behind state table.  */
  };

static struct random_data_t unsafe_state =
{
    .fptr = &randtbl[SEP_3 + 1],
    .rptr = &randtbl[1],
    .state = &randtbl[1],
    .rand_type = TYPE_3,
    .rand_deg = DEG_3,
    .rand_sep = SEP_3,
    .end_ptr = &randtbl[sizeof (randtbl) / sizeof (randtbl[0])]
};

int __random_r (struct random_data_t *buf, int32_t *result)
{
  int32_t *state;

  if (buf == NULL || result == NULL)
	  return -1;

  state = buf->state;

  if (buf->rand_type == TYPE_0)
    {
      int32_t val = ((state[0] * 1103515245U) + 12345U) & 0x7fffffff;
      state[0] = val;
      *result = val;
    }
  else
    {
      int32_t *fptr = buf->fptr;
      int32_t *rptr = buf->rptr;
      int32_t *end_ptr = buf->end_ptr;
      uint32_t val;

      val = *fptr += (uint32_t) *rptr;
      /* Chucking least random bit.  */
      *result = val >> 1;
      ++fptr;
      if (fptr >= end_ptr)
	{
	  fptr = state;
	  ++rptr;
	}
      else
	{
	  ++rptr;
	  if (rptr >= end_ptr)
	    rptr = state;
	}
      buf->fptr = fptr;
      buf->rptr = rptr;
    }
  return 0;
}

int __srandom_r (unsigned int seed, struct random_data_t *buf)
{
  int type;
  int32_t *state;
  long int i;
  int32_t word;
  int32_t *dst;
  int kc;

  if (buf == NULL)
	  return -1;
  type = buf->rand_type;
  if ((unsigned int) type >= MAX_TYPES)
	  return -1;

  state = buf->state;
  /* We must make sure the seed is not 0.  Take arbitrarily 1 in this case.  */
  if (seed == 0)
    seed = 1;
  state[0] = seed;
  if (type == TYPE_0)
    goto done;

  dst = state;
  word = seed;
  kc = buf->rand_deg;
  for (i = 1; i < kc; ++i)
    {
      /* This does:
	   state[i] = (16807 * state[i - 1]) % 2147483647;
	 but avoids overflowing 31 bits.  */
      long int hi = word / 127773;
      long int lo = word % 127773;
      word = 16807 * lo - 2836 * hi;
      if (word < 0)
	word += 2147483647;
      *++dst = word;
    }

  buf->fptr = &state[buf->rand_sep];
  buf->rptr = &state[0];
  kc *= 10;
  while (--kc >= 0)
    {
      int32_t discard;
      (void) __random_r (buf, &discard);
    }

 done:
  return 0;
}

int __initstate_r (unsigned int seed, char *arg_state, size_t n,
	       struct random_data_t *buf)
{
  if (buf == NULL)
	  return -1;

  int32_t *old_state = buf->state;
  if (old_state != NULL)
    {
      int old_type = buf->rand_type;
      if (old_type == TYPE_0)
	old_state[-1] = TYPE_0;
      else
	old_state[-1] = (MAX_TYPES * (buf->rptr - old_state)) + old_type;
    }

  int type;
  if (n >= BREAK_3)
    type = n < BREAK_4 ? TYPE_3 : TYPE_4;
  else if (n < BREAK_1)
    {
      if (n < BREAK_0)
		  return -1;

      type = TYPE_0;
    }
  else
    type = n < BREAK_2 ? TYPE_1 : TYPE_2;

  int degree = random_poly_info.degrees[type];
  int separation = random_poly_info.seps[type];

  buf->rand_type = type;
  buf->rand_sep = separation;
  buf->rand_deg = degree;
  int32_t *state = &((int32_t *) arg_state)[1];	/* First location.  */
  /* Must set END_PTR before srandom.  */
  buf->end_ptr = &state[degree];

  buf->state = state;

  __srandom_r (seed, buf);

  state[-1] = TYPE_0;
  if (type != TYPE_0)
    state[-1] = (buf->rptr - state) * MAX_TYPES + type;

  return 0;

}

int __setstate_r (char *arg_state, struct random_data_t *buf)
{
  int32_t *new_state = 1 + (int32_t *) arg_state;
  int type;
  int old_type;
  int32_t *old_state;
  int degree;
  int separation;

  if (arg_state == NULL || buf == NULL)
	  return -1;

  old_type = buf->rand_type;
  old_state = buf->state;
  if (old_type == TYPE_0)
    old_state[-1] = TYPE_0;
  else
    old_state[-1] = (MAX_TYPES * (buf->rptr - old_state)) + old_type;

  type = new_state[-1] % MAX_TYPES;
  if (type < TYPE_0 || type > TYPE_4)
	  return -1;

  buf->rand_deg = degree = random_poly_info.degrees[type];
  buf->rand_sep = separation = random_poly_info.seps[type];
  buf->rand_type = type;

  if (type != TYPE_0)
    {
      int rear = new_state[-1] / MAX_TYPES;
      buf->rptr = &new_state[rear];
      buf->fptr = &new_state[(rear + separation) % degree];
    }
  buf->state = new_state;
  /* Set end_ptr too.  */
  buf->end_ptr = &new_state[degree];

  return 0;
}


void __srandom (unsigned int x)
{
  (void) __srandom_r (x, &unsafe_state);
}

char *__initstate (unsigned int seed, char *arg_state, size_t n)
{
  int32_t *ostate;
  int ret;

  ostate = &unsafe_state.state[-1];

  ret = __initstate_r (seed, arg_state, n, &unsafe_state);

  return ret == -1 ? NULL : (char *) ostate;
}

char *__setstate (char *arg_state)
{
  int32_t *ostate;

  ostate = &unsafe_state.state[-1];

  if (__setstate_r (arg_state, &unsafe_state) < 0)
    ostate = NULL;

  return (char *) ostate;
}

long int __random (void)
{
  int32_t retval;

  (void) __random_r (&unsafe_state, &retval);

  return retval;
}
