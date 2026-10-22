/*
 * doctor.h -- `hull doctor`, what this host gives a user against what hull
 * can use (section 7).
 */

#ifndef HULL_DOCTOR_H
#define HULL_DOCTOR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Each check a line on stdout, `ok`, `missing`, `limited` or `absent`
 * first, and for what is missing the one-time fix after it. `runtime_why`
 * is why no runtime directory was chosen, where none was. 0, or 1 where
 * user namespaces or a runtime directory cannot be had. */
int hull_doctor(const char *runtime_why);

#ifdef __cplusplus
}
#endif

#endif /* HULL_DOCTOR_H */
