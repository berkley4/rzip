/* 
   Copyright (C) Andrew Tridgell 1998
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
  Utilities used in rzip

  tridge, June 1996
  */
#include "rzip.h"


void *Realloc(void *p, int size)
{
	if (!p) return (void *)malloc(size);
	return (void *)realloc(p, size);
}

void err_msg(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

void fatal(const char *format, ...)
{
	va_list ap;

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}


	fprintf(stderr, "Fatal error - exiting\n");
	exit(1);
}
