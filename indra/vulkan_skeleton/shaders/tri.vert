#version 450

// Attribute locations are the LLVertexBuffer::AttributeType values the viewer's shaders already use.
layout(location = 0) in vec3 position;       // TYPE_VERTEX
layout(location = 6) in vec4 diffuse_color;  // TYPE_COLOR

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = vec4(position, 1.0);
    vColor = diffuse_color;
}
