# SPDX-License-Identifier: GPL-2.0
lib-y		:= clear_user.o delay.o copy_from_user.o		\
		   copy_to_user.o copy_in_user.o copy_page.o		\
		   clear_page.o memchr.o memcpy.o memmove.o memset.o	\
		   memcmp.o strcmp.o strncmp.o strlen.o strnlen.o	\
		   strchr.o strrchr.o tishift.o

CFLAGS_atomic_ll_sc.o	:= -ffixed-x1 -ffixed-x2        		\
		   -ffixed-x3 -ffixed-x4 -ffixed-x5 -ffixed-x6		\
		   -ffixed-x7 -fcall-saved-x8 -fcall-saved-x9		\
		   -fcall-saved-x10 -fcall-saved-x11 -fcall-saved-x12	\
		   -fcall-saved-x13 -fcall-saved-x14 -fcall-saved-x15	\
		   -fcall-saved-x18 -fomit-frame-pointer
CFLAGS_REMOVE_atomic_ll_sc.o := -pg
GCOV_PROFILE_atomic_ll_sc.o	:= n
KASAN_SANITIZE_atomic_ll_sc.o	:= n
KCOV_INSTRUMENT_atomic_ll_sc.o	:= n
UBSAN_SANITIZE_atomic_ll_sc.o	:= n

lib-$(CONFIG_ARCH_HAS_UACCESS_FLUSHCACHE) += uaccess_flushcache.o
