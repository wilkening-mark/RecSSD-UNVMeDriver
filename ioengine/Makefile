#
# Copyright (c) 2015-2016, Micron Technology, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#   1. Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#
#   2. Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in the
#      documentation and/or other materials provided with the distribution.
#
#   3. Neither the name of the copyright holder nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

include ../Makefile.def

ifneq ($(wildcard $(FIODIR)/fio.h),)
	TARGET := unvme_fio
ifeq ($(shell grep -c io_ops_data $(FIODIR)/fio.h),0)
	CPPFLAGS += -Dio_ops_data="io_ops->data"
endif
endif

UNVME_SRC = ../src

CPPFLAGS += -I$(UNVME_SRC) -I$(FIODIR)
LDFLAGS += -shared -rdynamic
LDLIBS += -lpthread -lrt

OBJS = $(addsuffix .o, $(TARGET))

all: $(TARGET)

$(TARGET): $(UNVME_SRC)/libunvme.a

lint: CFLAGS = -Wall -D_FORTIFY_SOURCE=2 -DUNVME_DEBUG -O3
lint: clean $(OBJS)
	@$(RM) *.o

clean:
	$(RM) unvme_fio *.o

.PHONY: all lint clean

