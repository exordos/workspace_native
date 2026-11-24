Verification (measured on this lane's build, tip-based):

- RED (fix statements stripped, regression case kept): the seam case aborted
  the process 8/8 (SIGABRT x7, SIGSEGV x1) - deterministic pre-fix failure.
- GREEN (fix restored): seam case PASS 8/8.
- Seam loops on the fixed binary: InvalidEndpoint x16 green,
  SendsOnlyPrompt x16 green, profile exit-teardown x12 green (44/44).
- Bucket regen ran: 545 functions -> 24 window buckets, new case in part5,
  max bucket 113.1 s under the 120 s ceiling.

LIVE VERIFY: SKIPPED (tests-only change, no product code path touched).
