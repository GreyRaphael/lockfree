# lockfree

lockfree queue for C++20 and above

## Requirements

`vcpkg install doctest`


## SPSC optimization

Separate Head and Tail Indices:
- Producer only modifies head_ and reads tail_.
- Consumer only modifies tail_ and reads head_.
- This separation ensures that the producer and consumer do not contend over the same atomic variable, reducing cache coherence traffic.

Cache Line Padding:
- Aligning head_ and tail_ to separate cache lines (typically 64 bytes) prevents them from residing on the same cache line, which can cause false sharing and degrade performance.

Power-of-Two Buffer Size with Bitmasking:
- By ensuring that N is a power of two, we can replace the modulo operation with a bitmask (& mask_), which is significantly faster.
- The static_assert ensures at compile-time that N meets this requirement.

Avoiding std::optional in pop():
- While an optional return type provides a convenient interface, it introduces additional overhead.Instead, providing an output parameter version of pop can enhance performance.
- An additional pop() method returning std::optional<T> is included for convenience, but its usage should be limited to scenarios where the slight overhead is acceptable.

Exception Safety:
- The class is marked noexcept where appropriate, ensuring that it doesn't throw exceptions, which is crucial for lock-free data structures.