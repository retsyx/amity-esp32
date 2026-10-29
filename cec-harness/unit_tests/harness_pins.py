"""Read bench assignments from the same Markdown source as the framework."""

from functools import lru_cache

from amity_harness import Wiring


@lru_cache(maxsize=1)
def wiring():
    return Wiring.from_markdown()


def cec_gpio(name="amity-dut", tentacle=0, direction="back"):
    return wiring().cec_gpio(name, tentacle, direction)


def reset_gpio(name="amity-dut"):
    return wiring().reset_gpio(name)
