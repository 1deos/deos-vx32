#ifdef __APPLE__
#define EXT(s) _ ## s
#else
#define EXT(s) s
#endif

.global EXT(cld)
EXT(cld):
	cld
	ret

.global EXT(geteflags)
EXT(geteflags):
	pushfl
	movl	(%esp), %eax
	popfl
	ret
