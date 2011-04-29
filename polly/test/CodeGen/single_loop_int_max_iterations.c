#define N 20
#include "limits.h"

int main () {
  int i;
  int A[N];

  A[0] = 0;

  __sync_synchronize();

  for (i = 0; i < INT_MAX; i++)
    A[0] = i;

  __sync_synchronize();

  if (A[0] == INT_MAX - 1)
    return 0;
  else
    return 1;
}
