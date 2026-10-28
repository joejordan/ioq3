uniform sampler2D u_DiffuseMap;

uniform int       u_AlphaTest;

#if defined(USE_PER_PIXEL)
uniform vec4      u_DlightInfo;

varying vec3      var_LightDir;
varying vec3      var_Normal;
#else
varying vec2      var_Tex1;
#endif
varying vec4      var_Color;


void main()
{
#if defined(USE_PER_PIXEL)
	// as lightall lights a surface with a dlight (USE_LIGHT_VECTOR): full
	// out to the radius over the square root of 3, none from the radius on,
	// and by the angle the light meets the surface at
	float sqrLightDist = dot(var_LightDir, var_LightDir);
	float normDist = 1.0 / (sqrLightDist * u_DlightInfo.a * u_DlightInfo.a);
	float attenuation = clamp(0.5 * normDist - 0.5, 0.0, 1.0);
	vec3 N = normalize(!gl_FrontFacing ? var_Normal : -var_Normal);
	float NL = clamp(dot(N, var_LightDir) * inversesqrt(sqrLightDist), 0.0, 1.0);
	vec4 color = vec4(vec3(attenuation * NL), 1.0);
#else
	vec4 color = texture2D(u_DiffuseMap, var_Tex1);
#endif

	float alpha = color.a * var_Color.a;
	if (u_AlphaTest == 1)
	{
		if (alpha == 0.0)
			discard;
	}
	else if (u_AlphaTest == 2)
	{
		if (alpha >= 0.5)
			discard;
	}
	else if (u_AlphaTest == 3)
	{
		if (alpha < 0.5)
			discard;
	}
	
	gl_FragColor.rgb = color.rgb * var_Color.rgb;
	gl_FragColor.a = alpha;
}
