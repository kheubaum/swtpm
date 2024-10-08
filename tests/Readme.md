
This directory contains the swtpm tests.

To run the tests you need to build swtpm first and then you can run the
tests using the following command line:

```
SWTPM_TEST_PROFILE='{"Name":"default-v1"}' \
SWTPM_TEST_EXPENSIVE=1 SWTPM_TEST_STORE_VOLATILE=1 SWTPM_TEST_IBMTSS2=1 make check
```

You may omit the environment variables if you don't want to run
the more time-consuming tests.

`SWTPM_TEST_EXPENSIVE=1` enables the following tests:
 - test_tpm12
 - test_tpm2_ibmtss2
 - test_tpm2_libtpms_versions_profiles

`SWTPM_TEST_STORE_VOLATILE=1` enables storing and restoring of the volatile
state at every step of the test_tpm2_ibmtss2 test. This environment
variable only has an effect if `SWTPM_TEST_EXPENSIVE=1` is set.

`SWTPM_TEST_IBMTSS2=1` enables the following tests
 - test_tpm2_save_load_state_2
 - test_tpm2_save_load_state_3
 - test_tpm2_libtpms_versions_profiles

`SWTPM_TEST_PROFILE` allows to set a profile for the following tests:
 - test_tpm2_ibmtss2

Note: The test suite will terminate with an error if the profile disables an
algorithm that it requires.


To run against an installed IBM TSS test suite, you may set the
`SWTPM_TEST_IBMTSS` to the location of the test suite, such as
`/usr/libexec/installed-tests/ibmtss`.

Some tests require root rights, especially those involving the CUSE TPM
and the vTPM proxy device. To run these and all the other ones you
can use the following command line:

```
sudo bash -c "SWTPM_TEST_EXPENSIVE=1 SWTPM_TEST_STORE_VOLATILE=1 SWTPM_TEST_IBMTSS2=1 make check"
```
