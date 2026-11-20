| feature | pic12 | pic14 | pic18 | pic32 | esp32 |
|---|---|---|---|---|---|
| tag_fold | fold | fold | fold | fold | fold |
| masked_predicate | fold | fold | fold | fold | fold |
| logical_fold | fold | fold | fold | fold | fold |
| byref_view_const | refused | yes (26) | yes (58) | yes (119) | yes (329) |
| byref_view_runtime | n/a | yes (32) | yes (72) | yes (123) | yes (351) |
| indirect_addressing | refused | yes (16) | yes (34) | yes (115) | yes (276) |

N is the back end's code-size unit: words on the 12/14-bit
cores, bytes on the 18F and ESP32, instructions on PIC32.
`fold` means the folded witness emits exactly the hand twin's
instruction stream; `refused` means the compile fails with the
diagnostic pinned in core-feature-baseline.txt.
